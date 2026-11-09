#include "biofeedback_dev_tab.h"
#include "biofeedback_dev_view.h"
#include "biofeedback_timeline.h"
#include "widget_kit.h"
#include "../app_controller.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QAbstractSpinBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QAbstractButton>
#include <QKeyEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QDateTime>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QtMath>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

namespace {

constexpr float kRadToDeg = 57.29577951308232f;
const QColor kInputColor(150, 160, 175);
const QColor kTargetColor(130, 185, 255);
const QColor kOutputColor(242, 184, 75);   // amber = "modified" everywhere in this tab
const QColor kOriginalAccent(140, 152, 167);

bf::Vec3 jointPos(const TrackedBody& b, int j) {
    return {b.joints[j].x, b.joints[j].y, b.joints[j].z};
}

QString fmtTimecode(double t) {
    if (t < 0) t = 0;
    const int m = (int)(t / 60.0);
    const double s = t - m * 60.0;
    return QString("%1:%2").arg(m).arg(s, 6, 'f', 3, QChar('0'));
}

QString fmtDuration(double t) {
    const int total = (int)std::round(std::max(0.0, t));
    return QString("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QChar('0'));
}

QString fmtDeg(float rad) {
    if (!std::isfinite(rad)) return "—";
    float d = rad * kRadToDeg;
    if (std::fabs(d) < 0.05f) d = 0.0f;  // no "-0.0°"
    return QString::number(d, 'f', 1) + "°";
}

QString fmtMm(float m) {
    if (!std::isfinite(m)) return "—";
    float mm = m * 1000.0f;
    if (std::fabs(mm) < 0.5f) mm = 0.0f;  // no "-0 mm"
    return QString::number(mm, 'f', 0) + " mm";
}

bool finitePos(const Joint& j) {
    return std::isfinite(j.x) && std::isfinite(j.y) && std::isfinite(j.z);
}

QString errorColor(float metres) {
    const float mm = std::fabs(metres) * 1000.0f;
    if (mm < 5.0f) return "#62cc8e";
    if (mm < 20.0f) return "#f2b84b";
    return "#ff6767";
}

QLabel* mutedLabel(const QString& text) {
    auto* l = new QLabel(text);
    l->setProperty("muted", true);
    l->setWordWrap(true);
    return l;
}

QToolButton* toolButton(const QString& text, const QString& tip, bool checkable = false) {
    auto* b = new QToolButton;
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(checkable);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    b->setAutoRaise(false);
    b->setMinimumHeight(24);
    return b;
}

// Transport glyphs drawn in the app's light text colour (the Fusion standard
// media icons are near-black on this dark theme)
QIcon transportIcon(const QString& kind) {
    const int S = 64;
    QPixmap pm(S, S);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(220, 230, 238));
    auto tri = [&](double x0, double x1, double y0, double y1) {  // pointing from x0 to x1
        QPainterPath path;
        path.moveTo(x0, y0);
        path.lineTo(x1, (y0 + y1) / 2);
        path.lineTo(x0, y1);
        path.closeSubpath();
        p.drawPath(path);
    };
    if (kind == "play") tri(20, 50, 14, 50);
    else if (kind == "pause") { p.drawRoundedRect(QRectF(18, 14, 10, 36), 2, 2); p.drawRoundedRect(QRectF(36, 14, 10, 36), 2, 2); }
    else if (kind == "start") { p.drawRect(QRectF(12, 16, 6, 32)); tri(38, 18, 16, 48); tri(56, 36, 16, 48); }
    else if (kind == "end") { tri(8, 28, 16, 48); tri(26, 46, 16, 48); p.drawRect(QRectF(46, 16, 6, 32)); }
    else if (kind == "prev") { tri(40, 16, 16, 48); p.drawRect(QRectF(42, 16, 6, 32)); }
    else if (kind == "next") { p.drawRect(QRectF(16, 16, 6, 32)); tri(24, 48, 16, 48); }
    return QIcon(pm);
}

QToolButton* iconButton(const QString& kind, const QString& tip) {
    auto* b = new QToolButton;
    b->setIcon(transportIcon(kind));
    b->setToolTip(tip);
    b->setIconSize(QSize(18, 18));
    b->setMinimumSize(32, 28);
    return b;
}

// "<prefix>[_camN]_YYYYmmdd_HHMMSS[suffix].csv" → prefix + timestamp
bool parseSessionName(const QString& fileName, const QString& suffix, QString& prefix, QDateTime& when) {
    static const QRegularExpression base(R"(^(.*?)(?:_cam\d)?_(\d{8})_(\d{6})$)");
    const QString stem = fileName.endsWith(suffix) ? fileName.left(fileName.size() - suffix.size()) : QString();
    if (stem.isEmpty()) return false;
    const auto m = base.match(stem);
    if (!m.hasMatch()) return false;
    prefix = m.captured(1);
    when = QDateTime::fromString(m.captured(2) + m.captured(3), "yyyyMMddHHmmss");
    return when.isValid();
}

} // namespace

// Scrolling the controls panel must not change the combo box / spin box /
// slider that happens to be under the pointer: unless the control has been
// clicked (focused), its wheel events go to the panel instead.
class WheelGuard : public QObject {
public:
    WheelGuard(QScrollArea* area, QObject* parent) : QObject(parent), area_(area) {}

protected:
    bool eventFilter(QObject* obj, QEvent* e) override {
        if (e->type() != QEvent::Wheel) return false;
        auto* w = qobject_cast<QWidget*>(obj);
        if (!w || w->hasFocus()) return false;
        QCoreApplication::sendEvent(area_->viewport(), e);
        return true;
    }

private:
    QScrollArea* area_;
};

// =============================================================================
// Construction
// =============================================================================

BiofeedbackDevTab::BiofeedbackDevTab(AppController* ctrl, QWidget* parent)
    : QWidget(parent), ctrl_(ctrl) {
    if (ctrl_) smoothing_ = ctrl_->smoothingFactor;
    settings_.limbs[0].enabled = true;  // start with the left leg selected and active (gain 1 = no change)

    camA_ = std::make_shared<BfOrbitCamera>();
    camB_ = camA_;

    // Editor-style tool buttons (the app stylesheet only styles QPushButton)
    setStyleSheet(
        "QToolButton { background-color:#1e2733; border:1px solid #2c3644; border-radius:4px;"
        " padding:3px 9px; color:#dce6ee; }"
        "QToolButton:hover { background-color:#263140; border-color:#3a4757; }"
        "QToolButton:pressed { background-color:#18202a; }"
        "QToolButton:checked { background-color:#2b4f5a; border-color:#3f8fa0; color:#ffffff; }"
        "QToolButton#bfPlayButton { background-color:#2e6f7e; border-color:#3f8fa0; }"
        "QToolButton#bfPlayButton:hover { background-color:#35808f; }"
        "QListWidget { background-color:#121820; border:1px solid #2c3644; border-radius:4px; }"
        "QListWidget::item { padding:4px 2px; }"
        "QListWidget::item:selected { background-color:#233746; color:#ffffff; }");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);
    root->addWidget(buildToolbar());

    auto* vsplit = new QSplitter(Qt::Vertical);
    vsplit->setHandleWidth(5);
    vsplit->setChildrenCollapsible(false);

    // --- Viewports
    auto* viewsFrame = new QFrame;
    viewsFrame->setProperty("dsPanel", true);
    auto* viewsLayout = new QHBoxLayout(viewsFrame);
    viewsLayout->setContentsMargins(4, 4, 4, 4);
    viewsLayout->setSpacing(4);
    auto* hsplit = new QSplitter(Qt::Horizontal);
    hsplit->setHandleWidth(4);
    viewOriginal_ = new BiofeedbackDevView;
    viewModified_ = new BiofeedbackDevView;
    viewOriginal_->setCamera(camA_);
    viewModified_->setCamera(camB_);
    hsplit->addWidget(viewOriginal_);
    hsplit->addWidget(viewModified_);
    hsplit->setSizes({1, 1});
    viewsLayout->addWidget(hsplit);
    vsplit->addWidget(viewsFrame);

    auto onCameraEdited = [this](BiofeedbackDevView* other) {
        if (linked_) other->refresh();
    };
    connect(viewOriginal_, &BiofeedbackDevView::cameraEdited, this,
            [this, onCameraEdited] { onCameraEdited(viewModified_); });
    connect(viewModified_, &BiofeedbackDevView::cameraEdited, this,
            [this, onCameraEdited] { onCameraEdited(viewOriginal_); });
    connect(viewOriginal_, &BiofeedbackDevView::resetRequested, this, [this] { setCameraPreset("front"); });
    connect(viewModified_, &BiofeedbackDevView::resetRequested, this, [this] { setCameraPreset("front"); });

    // --- Transport + timeline
    auto* timeFrame = new QFrame;
    timeFrame->setProperty("dsPanel", true);
    auto* timeLayout = new QVBoxLayout(timeFrame);
    timeLayout->setContentsMargins(6, 4, 6, 6);
    timeLayout->setSpacing(4);
    timeLayout->addWidget(buildTransport());
    timeline_ = new BiofeedbackTimeline;
    timeline_->setToolTip("Click or drag to scrub · wheel to zoom · Shift+wheel to scroll · "
                          "double-click the ruler to fit the whole session");
    connect(timeline_, &BiofeedbackTimeline::seekRequested, this, [this](double t) {
        playTime_ = t;
        setCurrentFrame(frameIndexAt(t));
    });
    connect(timeline_, &BiofeedbackTimeline::scrubStarted, this, [this] { setPlaying(false); });
    timeLayout->addWidget(timeline_, 1);
    vsplit->addWidget(timeFrame);

    // --- Development controls (scrolls when the window is small)
    auto* controlsScroll = new QScrollArea;
    controlsScroll->setWidgetResizable(true);
    controlsScroll->setFrameShape(QFrame::NoFrame);
    controlsScroll->setWidget(buildControls());
    controlsScroll->setMinimumHeight(140);
    auto* guard = new WheelGuard(controlsScroll, this);
    for (QWidget* w : controlsScroll->widget()->findChildren<QWidget*>()) {
        if (qobject_cast<QComboBox*>(w) || qobject_cast<QAbstractSpinBox*>(w) || qobject_cast<QSlider*>(w)) {
            w->setFocusPolicy(Qt::StrongFocus);  // not WheelFocus: the wheel alone doesn't grab it
            w->installEventFilter(guard);
        }
    }
    vsplit->addWidget(controlsScroll);

    vsplit->setStretchFactor(0, 5);
    vsplit->setStretchFactor(1, 3);
    vsplit->setStretchFactor(2, 0);
    vsplit->setSizes({430, 250, 250});
    root->addWidget(vsplit, 1);

    playTimer_ = new QTimer(this);
    playTimer_->setTimerType(Qt::PreciseTimer);
    playTimer_->setInterval(15);
    connect(playTimer_, &QTimer::timeout, this, &BiofeedbackDevTab::onTick);
    clock_.start();

    recomputeTimer_ = new QTimer(this);
    recomputeTimer_->setSingleShot(true);
    recomputeTimer_->setInterval(30);
    connect(recomputeTimer_, &QTimer::timeout, this, &BiofeedbackDevTab::recompute);

    // Transport keys are handled in keyPressEvent, so a focused slider or spin
    // box keeps its own arrow keys. Buttons don't take focus, so Space can't
    // re-click the last button pressed (e.g. "Neutral = this frame").
    setFocusPolicy(Qt::ClickFocus);
    for (auto* btn : findChildren<QAbstractButton*>()) btn->setFocusPolicy(Qt::NoFocus);
    transport_->setEnabled(false);

    syncLimbList();
    syncInspector();
    syncEngineCombo();
    refreshFrame();
}

BiofeedbackDevTab::~BiofeedbackDevTab() = default;

QWidget* BiofeedbackDevTab::buildToolbar() {
    auto* bar = new QFrame;
    bar->setProperty("dsPanel", true);
    auto* l = new QHBoxLayout(bar);
    l->setContentsMargins(8, 5, 8, 5);
    l->setSpacing(8);

    auto* title = new QLabel("BIOFEEDBACK DEVELOPMENT");
    title->setProperty("dsPanelTitle", true);
    l->addWidget(title);
    auto* exp = WidgetKit::statusPill("EXPERIMENTAL");
    exp->setProperty("dsStatusPill", "warning");
    l->addWidget(exp);

    auto* openBtn = WidgetKit::button("Open Session...", WidgetKit::ButtonRole::Primary);
    connect(openBtn, &QPushButton::clicked, this, [this] {
        QString dir = sessionPath_.isEmpty() ? QString() : QFileInfo(sessionPath_).absolutePath();
        if (dir.isEmpty() && ctrl_) dir = QString::fromStdString(ctrl_->config().recordingsDir);
        const QString path = QFileDialog::getOpenFileName(this, "Open recorded session", dir,
                                                          "Joint recordings (*.csv)");
        if (!path.isEmpty()) loadSession(path);
    });
    l->addWidget(openBtn);

    sessionLabel_ = new QLabel("No session loaded");
    sessionLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);  // elide, don't push
    sessionLabel_->setProperty("muted", true);
    sessionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->addWidget(sessionLabel_, 1);

    dataNoteLabel_ = WidgetKit::statusPill();
    dataNoteLabel_->setVisible(false);
    l->addWidget(dataNoteLabel_);

    for (auto [text, key, tip] : {std::tuple{"Front", "front", "View from in front of the subject"},
                                  std::tuple{"Left", "left", "View the subject's left side"},
                                  std::tuple{"Right", "right", "View the subject's right side"},
                                  std::tuple{"Top", "top", "View from above"}}) {
        auto* b = toolButton(text, tip);
        const QString k = key;
        connect(b, &QToolButton::clicked, this, [this, k] { setCameraPreset(k); });
        l->addWidget(b);
    }

    linkBtn_ = toolButton("Link views", "Orbit / pan / zoom both viewports together", true);
    linkBtn_->setChecked(true);
    connect(linkBtn_, &QToolButton::toggled, this, [this](bool on) {
        linked_ = on;
        camB_ = on ? camA_ : std::make_shared<BfOrbitCamera>(*camA_);
        viewModified_->setCamera(camB_);
        refreshFrame();
    });
    l->addWidget(linkBtn_);

    followBtn_ = toolButton("Follow body", "Keep the pelvis centred (off: fixed camera, see travel)", true);
    followBtn_->setChecked(true);
    connect(followBtn_, &QToolButton::toggled, this, [this](bool on) {
        follow_ = on;
        refreshFrame();
    });
    l->addWidget(followBtn_);
    return bar;
}

QWidget* BiofeedbackDevTab::buildTransport() {
    auto* w = new QWidget;
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(4);

    auto* startBtn = iconButton("start", "Go to start (Home)");
    auto* backBtn = iconButton("prev", "Previous frame (Left, Shift+Left = 10)");
    playBtn_ = iconButton("play", "Play / pause (Space)");
    playBtn_->setObjectName("bfPlayButton");
    playBtn_->setMinimumSize(44, 28);
    auto* fwdBtn = iconButton("next", "Next frame (Right, Shift+Right = 10)");
    auto* endBtn = iconButton("end", "Go to end (End)");
    connect(startBtn, &QToolButton::clicked, this, [this] { setCurrentFrame(0); });
    connect(backBtn, &QToolButton::clicked, this, [this] { stepFrames(-1); });
    connect(playBtn_, &QToolButton::clicked, this, [this] { setPlaying(!playing_); });
    connect(fwdBtn, &QToolButton::clicked, this, [this] { stepFrames(1); });
    connect(endBtn, &QToolButton::clicked, this, [this] { setCurrentFrame((int)times_.size() - 1); });
    for (auto* b : {startBtn, backBtn, playBtn_, fwdBtn, endBtn}) l->addWidget(b);

    loopBtn_ = toolButton("Loop", "Loop playback", true);
    loopBtn_->setChecked(loop_);
    connect(loopBtn_, &QToolButton::toggled, this, [this](bool on) { loop_ = on; });
    l->addSpacing(6);
    l->addWidget(loopBtn_);

    speedCombo_ = new QComboBox;
    for (auto [text, v] : {std::pair{"0.1x", 0.1}, std::pair{"0.25x", 0.25}, std::pair{"0.5x", 0.5},
                           std::pair{"1x", 1.0}, std::pair{"1.5x", 1.5}, std::pair{"2x", 2.0}})
        speedCombo_->addItem(text, v);
    speedCombo_->setCurrentIndex(3);
    speedCombo_->setToolTip("Playback speed");
    connect(speedCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { speed_ = (float)speedCombo_->currentData().toDouble(); });
    l->addWidget(speedCombo_);

    l->addSpacing(14);
    timecodeLabel_ = new QLabel(fmtTimecode(0));
    QFont tc("Consolas");
    tc.setStyleHint(QFont::Monospace);
    tc.setPointSizeF(13.0);
    tc.setBold(true);
    timecodeLabel_->setFont(tc);
    timecodeLabel_->setStyleSheet("color:#f2b84b;");
    l->addWidget(timecodeLabel_);
    durationLabel_ = new QLabel("/ " + fmtTimecode(0));
    QFont dc = tc;
    dc.setPointSizeF(10.0);
    dc.setBold(false);
    durationLabel_->setFont(dc);
    durationLabel_->setProperty("muted", true);
    l->addWidget(durationLabel_);
    l->addSpacing(10);
    frameLabel_ = new QLabel;
    frameLabel_->setProperty("muted", true);
    l->addWidget(frameLabel_);
    l->addStretch();

    // View toggles live here, beside the viewports they affect
    ghostBtn_ = toolButton("Ghost", "Draw the original skeleton faintly behind the modified one", true);
    axesBtn_ = toolButton("Axes", "Show the anatomical axes the engine uses (R = subject's right, ANT = forward)", true);
    labelsBtn_ = toolButton("Labels", "Show the selected leg's joint angles on the skeletons", true);
    ghostBtn_->setChecked(showGhost_);
    axesBtn_->setChecked(showAxes_);
    labelsBtn_->setChecked(showLabels_);
    connect(ghostBtn_, &QToolButton::toggled, this, [this](bool on) { showGhost_ = on; refreshFrame(); });
    connect(axesBtn_, &QToolButton::toggled, this, [this](bool on) { showAxes_ = on; refreshFrame(); });
    connect(labelsBtn_, &QToolButton::toggled, this, [this](bool on) { showLabels_ = on; refreshFrame(); });
    l->addWidget(ghostBtn_);
    l->addWidget(axesBtn_);
    l->addWidget(labelsBtn_);
    transport_ = w;
    return w;
}

BiofeedbackDevTab::GainRow BiofeedbackDevTab::makeGainRow(const QString& label, const QString& tip,
                                                          float anat::LimbSettings::*field) {
    GainRow r;
    r.row = new QWidget;
    auto* l = new QHBoxLayout(r.row);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(6);
    auto* name = new QLabel(label);
    name->setToolTip(tip);
    name->setMinimumWidth(104);
    l->addWidget(name);
    r.slider = new QSlider(Qt::Horizontal);
    r.slider->setRange(0, 300);
    r.slider->setPageStep(10);
    r.slider->setToolTip(tip);
    r.slider->setMinimumWidth(90);
    l->addWidget(r.slider, 1);
    r.spin = new QDoubleSpinBox;
    r.spin->setRange(0.0, 3.0);
    r.spin->setSingleStep(0.05);
    r.spin->setDecimals(2);
    r.spin->setSuffix(" x");
    r.spin->setFixedWidth(76);
    l->addWidget(r.spin);
    auto* reset = toolButton("1x", "Reset to 1x (no change)");
    l->addWidget(reset);

    QSlider* slider = r.slider;
    QDoubleSpinBox* spin = r.spin;
    auto apply = [this, field](double v) {
        settings_.limbs[selectedLimb_].*field = (float)v;
        syncLimbList();
        scheduleRecompute();
    };
    connect(slider, &QSlider::valueChanged, this, [spin, apply](int v) {
        QSignalBlocker b(spin);
        spin->setValue(v / 100.0);
        apply(v / 100.0);
    });
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [slider, apply](double v) {
        QSignalBlocker b(slider);
        slider->setValue((int)std::lround(v * 100.0));
        apply(v);
    });
    connect(reset, &QToolButton::clicked, this, [spin] { spin->setValue(1.0); });
    return r;
}

QWidget* BiofeedbackDevTab::buildControls() {
    auto* panel = new QFrame;
    panel->setProperty("dsPanel", true);
    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(8, 6, 8, 8);
    outer->setSpacing(4);
    auto* title = new QLabel("DEVELOPMENT CONTROLS");
    title->setProperty("dsPanelTitle", true);
    outer->addWidget(title);

    auto* cols = new QHBoxLayout;
    cols->setSpacing(14);
    outer->addLayout(cols, 1);

    auto column = [&](const QString& header, int stretch) {
        auto* w = new QWidget;
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(5);
        WidgetKit::addSectionHeader(v, header);
        cols->addWidget(w, stretch);
        return v;
    };

    // --- Limbs (like tracks in an editor)
    auto* limbs = column("Limbs", 2);
    limbList_ = new QListWidget;
    limbList_->setMinimumWidth(150);
    limbList_->setMaximumHeight(110);
    for (int s = 0; s < 2; ++s) {
        auto* item = new QListWidgetItem;
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        limbList_->addItem(item);
    }
    limbList_->setCurrentRow(0);
    connect(limbList_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0) return;
        selectedLimb_ = row;
        syncInspector();
        rebuildTracks();
        refreshFrame();
    });
    connect(limbList_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        const int row = limbList_->row(item);
        if (row < 0 || row > 1) return;
        const bool on = item->checkState() == Qt::Checked;
        if (settings_.limbs[row].enabled == on) return;
        settings_.limbs[row].enabled = on;
        syncLimbList();
        scheduleRecompute();
    });
    limbs->addWidget(limbList_);
    limbs->addWidget(mutedLabel("Tick a limb to modify it; select it to edit and plot it."));
    auto* copyBtn = WidgetKit::button("Copy to other leg");
    copyBtn->setToolTip("Copy this leg's mode and gains to the other leg");
    connect(copyBtn, &QPushButton::clicked, this, [this] {
        const int other = 1 - selectedLimb_;
        const bool wasEnabled = settings_.limbs[other].enabled;
        settings_.limbs[other] = settings_.limbs[selectedLimb_];
        settings_.limbs[other].enabled = wasEnabled || settings_.limbs[selectedLimb_].enabled;
        syncLimbList();
        scheduleRecompute();
    });
    limbs->addWidget(copyBtn);
    limbs->addStretch();

    // --- Inspector
    auto* insp = column("Limb inspector", 4);
    inspectorTitle_ = new QLabel;
    inspectorTitle_->setStyleSheet("font-weight:650; color:#f2b84b;");
    insp->addWidget(inspectorTitle_);

    auto* modeRow = new QWidget;
    modeRow->setProperty("segmented", true);
    auto* modeLayout = new QHBoxLayout(modeRow);
    modeLayout->setContentsMargins(7, 2, 7, 2);
    modeLayout->setSpacing(14);
    modeAnglesRb_ = new QRadioButton("Joint angles");
    modeAnglesRb_->setToolTip("Scale each joint's flexion about neutral");
    modeStepRb_ = new QRadioButton("Step length");
    modeStepRb_->setToolTip("Scale how far the ankle travels ahead of / behind the hip;\n"
                            "hip and knee are solved by two-bone IK");
    auto* modeGroup = new QButtonGroup(this);
    modeGroup->addButton(modeAnglesRb_);
    modeGroup->addButton(modeStepRb_);
    modeLayout->addWidget(new QLabel("Mode:"));
    modeLayout->addWidget(modeAnglesRb_);
    modeLayout->addWidget(modeStepRb_);
    modeLayout->addStretch();
    insp->addWidget(modeRow);
    connect(modeStepRb_, &QRadioButton::toggled, this, [this](bool step) {
        settings_.limbs[selectedLimb_].mode = step ? anat::LimbMode::StepLength : anat::LimbMode::JointAngles;
        syncInspector();
        syncLimbList();
        rebuildTracks();
        scheduleRecompute();
    });

    hipRow_ = makeGainRow("Hip flexion", "Gain on hip flexion/extension about neutral", &anat::LimbSettings::hipGain);
    kneeRow_ = makeGainRow("Knee flexion", "Gain on knee flexion about neutral", &anat::LimbSettings::kneeGain);
    stepRow_ = makeGainRow("Step length", "Gain on the ankle's forward/back distance from the hip",
                           &anat::LimbSettings::stepGain);
    ankleRow_ = makeGainRow("Ankle dorsiflexion", "Gain on ankle dorsi/plantarflexion about neutral\n"
                            "(depth cameras track the ankle poorly)", &anat::LimbSettings::ankleGain);
    for (auto* r : {hipRow_.row, kneeRow_.row, stepRow_.row, ankleRow_.row}) insp->addWidget(r);

    auto* neutralRow = new QHBoxLayout;
    neutralRow->setSpacing(6);
    neutralLabel_ = mutedLabel("");
    neutralRow->addWidget(neutralLabel_, 1);
    auto* setNeutral = WidgetKit::button("Neutral = this frame");
    setNeutral->setToolTip("Use the current frame (ideally quiet standing) as neutral for both legs.\n"
                           "Gains scale the movement AWAY from neutral, so standing still is not distorted.");
    connect(setNeutral, &QPushButton::clicked, this, [this] {
        if (cur_ < 0 || cur_ >= (int)input_.size() || !hasBody_[cur_]) return;
        anat::captureNeutral(input_[cur_], settings_);
        syncInspector();
        scheduleRecompute();
    });
    auto* zeroNeutral = WidgetKit::button("Zero");
    zeroNeutral->setToolTip("Neutral = anatomical zero (straight leg, foot perpendicular)");
    connect(zeroNeutral, &QPushButton::clicked, this, [this] {
        settings_.neutral = {};
        syncInspector();
        scheduleRecompute();
    });
    neutralRow->addWidget(setNeutral);
    neutralRow->addWidget(zeroNeutral);
    insp->addLayout(neutralRow);
    insp->addStretch();

    // --- Engine
    auto* eng = column("Engine", 3);
    auto* engineRow = new QHBoxLayout;
    engineRow->addWidget(new QLabel("Engine:"));
    engineCombo_ = new QComboBox;
    engineCombo_->addItem("Anatomical (new)");
    engineCombo_->addItem("Current live engine");
    engineCombo_->addItem("Recorded avatar (what the patient saw)");
    engineCombo_->addItem("Anatomical: live preview (frame by frame)");
    engineCombo_->setToolTip("Which engine produces the MODIFIED view.\n"
                             "'Live preview' runs the anatomical engine exactly as the live patient\n"
                             "screen does: frame by frame, learning the floor, forward axis and\n"
                             "left/right swaps as it goes, with no look-ahead.\n"
                             "'Current live engine' uses the same gains as hip/knee/ankle scales\n"
                             "in the existing BiofeedbackEngine, for A/B comparison.");
    connect(engineCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        engine_ = (Engine)i;
        scheduleRecompute();
    });
    engineRow->addWidget(engineCombo_, 1);
    eng->addLayout(engineRow);

    auto* avatarRow = new QHBoxLayout;
    avatarStatus_ = mutedLabel("No recorded avatar");
    avatarRow->addWidget(avatarStatus_, 1);
    auto* avatarBtn = WidgetKit::button("Load avatar...");
    avatarBtn->setToolTip("Load a _biofeedback_avatar.csv recorded during a live biofeedback session");
    connect(avatarBtn, &QPushButton::clicked, this, [this] {
        const QString dir = sessionPath_.isEmpty() ? QString() : QFileInfo(sessionPath_).absolutePath();
        const QString path = QFileDialog::getOpenFileName(this, "Open recorded biofeedback avatar", dir,
                                                          "Biofeedback avatar (*_biofeedback_avatar.csv);;CSV (*.csv)");
        if (!path.isEmpty()) loadRecordedAvatar(path);
    });
    avatarRow->addWidget(avatarBtn);
    eng->addLayout(avatarRow);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);
    grid->addWidget(new QLabel("Smoothing:"), 0, 0);
    smoothingSpin_ = new QDoubleSpinBox;
    smoothingSpin_->setRange(0.0, 0.95);
    smoothingSpin_->setSingleStep(0.05);
    smoothingSpin_->setDecimals(2);
    smoothingSpin_->setValue(smoothing_);
    smoothingSpin_->setToolTip("Same exponential smoothing the live patient screen applies before biofeedback");
    connect(smoothingSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        smoothing_ = (float)v;
        rebuildInput();
        updateForwardSign();
        scheduleRecompute();
    });
    grid->addWidget(smoothingSpin_, 0, 1);

    grid->addWidget(new QLabel("Hip reference:"), 1, 0);
    hipRefCombo_ = new QComboBox;
    hipRefCombo_->addItem("Pelvis (anatomical, noisier)");
    hipRefCombo_->addItem("Floor vertical");
    hipRefCombo_->setCurrentIndex((int)settings_.hipReference);
    hipRefCombo_->setToolTip("Measure hip flexion against the floor's vertical (default) or the pelvis.\n"
                             "The pelvis axis comes from two joints ~20 cm apart (pelvis -> navel), so\n"
                             "tracking jitter tilts it by degrees per frame; the floor vertical is steady\n"
                             "and ignores trunk lean.");
    connect(hipRefCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        settings_.hipReference = (anat::HipReference)i;
        scheduleRecompute();
    });
    grid->addWidget(hipRefCombo_, 1, 1);

    grid->addWidget(new QLabel("Forward axis:"), 2, 0);
    forwardCombo_ = new QComboBox;
    forwardCombo_->addItem("Auto");
    forwardCombo_->addItem("Normal (+)");
    forwardCombo_->addItem("Flipped (-)");
    forwardCombo_->setToolTip("Which way is anterior. Auto-detected from the direction the feet point;\n"
                              "flip it if flexion and extension come out swapped.");
    connect(forwardCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        forwardMode_ = i;
        updateForwardSign();
        scheduleRecompute();
    });
    grid->addWidget(forwardCombo_, 2, 1);

    grid->addWidget(new QLabel("Floor:"), 3, 0);
    floorCombo_ = new QComboBox;
    floorCombo_->addItem("Auto");
    floorCombo_->addItem("Camera Y (level)");
    floorCombo_->setToolTip("The floor's vertical, used for ground contact, pelvis drop and the floor\n"
                            "hip reference. Auto fits a tilted floor when the subject walked >= 1.5 m\n"
                            "(camera pitched down); on a treadmill it assumes a level camera.");
    connect(floorCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        floorMode_ = i;
        applyFloorMode();
        updateForwardSign();
        scheduleRecompute();
    });
    grid->addWidget(floorCombo_, 3, 1);

    grid->addWidget(new QLabel("Locomotion:"), 4, 0);
    locomotionCombo_ = new QComboBox;
    locomotionCombo_->addItem("Auto");
    locomotionCombo_->addItem("Treadmill / in place");
    locomotionCombo_->addItem("Overground: plant stance foot");
    locomotionCombo_->setToolTip("Overground, the modified avatar's stance foot is kept fixed on the floor\n"
                                 "and the whole avatar travels further than the person (a scaled stance\n"
                                 "leg would otherwise slide backward). On a treadmill the real stance foot\n"
                                 "moves with the belt, so nothing is planted. Auto: overground when the\n"
                                 "pelvis travels more than 1.2 m within 4 s.");
    connect(locomotionCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        locomotionMode_ = i;
        scheduleRecompute();
    });
    grid->addWidget(locomotionCombo_, 4, 1);
    eng->addLayout(grid);

    groundCb_ = new QCheckBox("Ground contact (keep lowest foot point at its height)");
    groundCb_->setChecked(settings_.groundContact);
    connect(groundCb_, &QCheckBox::toggled, this, [this](bool on) {
        settings_.groundContact = on;
        scheduleRecompute();
    });
    eng->addWidget(groundCb_);
    fillGapsCb_ = new QCheckBox("Fill tracking gaps up to 0.25 s");
    fillGapsCb_->setChecked(fillGaps_);
    fillGapsCb_->setToolTip("Interpolate short dropouts. An unfilled dropout makes that frame's leg\n"
                            "unmeasurable (it snaps back to the original) and changes which foot point\n"
                            "is lowest, so the avatar jumps.");
    connect(fillGapsCb_, &QCheckBox::toggled, this, [this](bool on) {
        fillGaps_ = on;
        rebuildInput();
        updateForwardSign();
        scheduleRecompute();
    });
    eng->addWidget(fillGapsCb_);
    fixSwapsCb_ = new QCheckBox("Fix tracker left/right swaps (subject facing away)");
    fixSwapsCb_->setChecked(fixSwaps_);
    fixSwapsCb_->setToolTip("Azure Kinect assumes the subject faces the camera; when they turn their\n"
                            "back it labels their right side 'left'. Frames whose feet point the wrong\n"
                            "way for 0.5 s or more get their left/right labels swapped back.");
    connect(fixSwapsCb_, &QCheckBox::toggled, this, [this](bool on) {
        fixSwaps_ = on;
        if (!sessionPath_.isEmpty()) {
            const double t = cur_ < (int)times_.size() ? times_[cur_] : 0.0;
            loadSession(sessionPath_);
            seekToTime(t);
        }
    });
    eng->addWidget(fixSwapsCb_);
    smoothVerticalCb_ = new QCheckBox("Limit vertical corrections to 0.5 m/s");
    smoothVerticalCb_->setChecked(smoothVertical_);
    smoothVerticalCb_->setToolTip("Rate-limit ground shift and pelvis drop between frames so the avatar\n"
                                  "can't jump vertically when tracking flickers.");
    connect(smoothVerticalCb_, &QCheckBox::toggled, this, [this](bool on) {
        smoothVertical_ = on;
        scheduleRecompute();
    });
    eng->addWidget(smoothVerticalCb_);

    pelvisDropCb_ = new QCheckBox("Step mode: lower pelvis when a step is out of reach");
    pelvisDropCb_->setChecked(settings_.pelvisDrop);
    connect(pelvisDropCb_, &QCheckBox::toggled, this, [this](bool on) {
        settings_.pelvisDrop = on;
        scheduleRecompute();
    });
    eng->addWidget(pelvisDropCb_);
    useLiveCb_ = new QCheckBox("Use these settings for live biofeedback");
    useLiveCb_->setToolTip("The live patient screen switches to the anatomical engine with this tab's\n"
                           "settings (neutral, ground contact, step mode, gap filling, ...). Protocol\n"
                           "'Set BF Transform' events still set the hip/knee/ankle gains and ramps.\n"
                           "Capture neutral live (Live tab) for the patient in front of the camera.");
    useLiveCb_->setEnabled(ctrl_ != nullptr);
    useLiveCb_->setChecked(ctrl_ && ctrl_->useAnatomicalBiofeedback());
    connect(useLiveCb_, &QCheckBox::toggled, this, [this](bool on) {
        if (!ctrl_) return;
        if (on) pushLiveProfile();
        ctrl_->setUseAnatomicalBiofeedback(on);
    });
    eng->addWidget(useLiveCb_);

    eng->addStretch();

    // --- Readout
    auto* ro = column("This frame", 3);
    readout_ = new QLabel;
    readout_->setTextFormat(Qt::RichText);
    readout_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    readout_->setWordWrap(true);
    readout_->setMinimumWidth(200);
    ro->addWidget(readout_, 1);
    return panel;
}

// =============================================================================
// Session loading
// =============================================================================

bool BiofeedbackDevTab::loadSession(const QString& pickedPath) {
    setPlaying(false);
    const QString path = resolveSessionPath(pickedPath);
    PlaybackManager pm;
    if (!pm.loadCSV(path.toStdString()) || pm.frameCount() == 0) {
        QMessageBox::warning(this, "Biofeedback Development",
                             "Could not load joint data from:\n" + path +
                             "\n\nChoose a joint recording (columns time_seconds, body_id, joint_index, ...).");
        return false;
    }
    session_ = std::move(pm);
    sessionPath_ = path;
    const int n = session_.frameCount();

    // The subject is the body seen in the most frames. Trackers issue a new id
    // when they lose and re-find someone, so where that id is missing follow the
    // body nearest the subject's last pelvis position; a bystander further away
    // is ignored.
    std::map<uint32_t, int> counts;
    for (int i = 0; i < n; ++i)
        for (const auto& b : session_.getFrame(i)->bodies) counts[b.id]++;
    primaryBodyId_ = 0;
    int best = -1;
    for (auto [id, c] : counts)
        if (c > best) { best = c; primaryBodyId_ = id; }

    auto pelvisOf = [](const TrackedBody& b, bf::Vec3& out) {
        const Joint& p = b.joints[(int)CanonicalJoint::PELVIS];
        if (p.confidence < 0.1f || !finitePos(p)) return false;
        out = bf::Vec3(p.x, p.y, p.z);
        return true;
    };
    times_.assign(n, 0.0);
    raw_.assign(n, TrackedBody{});
    hasBody_.assign(n, 0);
    bodySwitches_ = 0;
    bool haveLast = false;
    bf::Vec3 lastPelvis;
    uint32_t lastId = primaryBodyId_;
    for (int i = 0; i < n; ++i) {
        const PlaybackFrame* f = session_.getFrame(i);
        times_[i] = f->timeSeconds;
        const TrackedBody* chosen = nullptr;
        for (const auto& b : f->bodies)
            if (b.id == primaryBodyId_) { chosen = &b; break; }
        if (!chosen) {
            float bestDist = 0.6f;
            for (const auto& b : f->bodies) {
                bf::Vec3 p;
                if (!pelvisOf(b, p)) continue;
                if (!haveLast) {
                    if (f->bodies.size() == 1) chosen = &b;
                    continue;
                }
                const float d = bf::length(p - lastPelvis);
                if (d < bestDist) { bestDist = d; chosen = &b; }
            }
        }
        if (!chosen) continue;
        raw_[i] = *chosen;
        raw_[i].id = primaryBodyId_;  // same person: keep one id so temporal state carries over
        hasBody_[i] = 1;
        if (chosen->id != lastId) { ++bodySwitches_; lastId = chosen->id; }
        bf::Vec3 p;
        if (pelvisOf(*chosen, p)) { lastPelvis = p; haveLast = true; }
    }

    // Centre and 2D detection
    double cx = 0, cy = 0, cz = 0;
    int centreCount = 0, planarVotes = 0, bodyCount = 0;
    std::vector<const TrackedBody*> sample;
    const int stride = std::max(1, n / 5000);
    for (int i = 0; i < n; ++i) {
        if (!hasBody_[i]) continue;
        const TrackedBody& b = raw_[i];
        bf::Vec3 p;
        if (pelvisOf(b, p)) { cx += p.x; cy += p.y; cz += p.z; ++centreCount; }
        bool flat = true, any = false;
        for (const auto& j : b.joints) {
            if (j.confidence < 0.1f || !finitePos(j)) continue;
            any = true;
            if (std::fabs(j.z) > 1e-5f) { flat = false; break; }
        }
        if (any) {
            planarVotes += flat ? 1 : 0;
            ++bodyCount;
        }
        if (i % stride == 0) sample.push_back(&b);
    }
    sessionCentre_ = centreCount ? bf::Vec3((float)(cx / centreCount), (float)(cy / centreCount), (float)(cz / centreCount))
                                 : bf::Vec3();
    planar_ = bodyCount > 0 && planarVotes * 2 > bodyCount;
    haveLastTarget_ = false;

    // Undo tracker left/right swaps (3D only): stretches of >= 0.5 s whose
    // feet vote against the session's forward axis had their sides swapped
    rawOriginal_ = raw_;
    swapped_.assign(n, 0);
    swappedSeconds_ = 0.0;
    if (!planar_ && fixSwaps_ && n > 1) {
        std::vector<const TrackedBody*> all(n, nullptr);
        for (int i = 0; i < n; ++i)
            if (hasBody_[i]) all[i] = &raw_[i];
        const int sessionSign = anat::detectForwardSign(all);
        if (sessionSign != 0) {
            const double dur0 = times_.back() > 0 ? times_.back() : 1.0;
            const int window = std::max(9, (int)std::lround((n - 1) / dur0));
            const std::vector<int> perFrame = anat::forwardSignsPerFrame(all, sessionSign, window);
            int runStart = -1;
            for (int i = 0; i <= n; ++i) {
                const bool against = i < n && hasBody_[i] && perFrame[i] != sessionSign;
                if (against && runStart < 0) runStart = i;
                if (!against && runStart >= 0) {
                    if (times_[i - 1] - times_[runStart] >= 0.5) {
                        for (int k = runStart; k < i; ++k) {
                            anat::swapLeftRight(raw_[k]);
                            swapped_[k] = 1;
                        }
                        swappedSeconds_ += times_[i - 1] - times_[runStart];
                    }
                    runStart = -1;
                }
            }
        }
    }

    // Floor plane (the camera may be pitched) and forward axis from the feet
    floorFitted_ = anat::estimateFloor(sample);
    floorLevel_ = anat::FloorPlane{};
    {
        std::vector<float> lows;
        for (const TrackedBody* b : sample) {
            float low = std::numeric_limits<float>::max();
            for (int j : {20, 21, 24, 25})
                if (b->joints[j].confidence >= 0.1f && finitePos(b->joints[j])) low = std::min(low, b->joints[j].y);
            if (low < std::numeric_limits<float>::max()) lows.push_back(low);
        }
        if (!lows.empty()) {
            std::sort(lows.begin(), lows.end());
            floorLevel_.valid = true;
            floorLevel_.height = lows[lows.size() / 50];
        }
    }
    applyFloorMode();
    detectedForward_ = anat::detectForwardSign(sample);
    forwardMode_ = 0;
    {
        QSignalBlocker block(forwardCombo_);
        forwardCombo_->setCurrentIndex(0);
    }

    settings_.neutral = {};
    avatarLoaded_ = false;
    avatar_.clear();
    avatarPath_.clear();
    rebuildInput();
    updateForwardSign();
    detectLocomotion();

    // Session info
    const double dur = times_.empty() ? 0.0 : times_.back();
    const double fps = dur > 0 ? (n - 1) / dur : 0.0;
    QStringList info{QFileInfo(path).fileName(), fmtDuration(dur), QString("%1 frames").arg(n),
                     QString("%1 fps").arg(fps, 0, 'f', 1), planar_ ? "2D (RGB)" : "3D"};
    if (floor_.valid && floor_.tiltDeg >= 3.0f) info << QString("floor tilt %1°").arg(floor_.tiltDeg, 0, 'f', 0);
    if (gapsFilled_ > 0) info << QString("%1 gaps filled").arg(gapsFilled_);
    if (bodySwitches_ > 0) info << QString("tracking id changed %1x").arg(bodySwitches_);
    if (swappedSeconds_ > 0.0) info << QString("left/right swap fixed for %1 s").arg(swappedSeconds_, 0, 'f', 1);
    sessionLabel_->setText(info.join("   ·   "));
    sessionLabel_->setToolTip(path);

    QString note;
    if (planar_) note = "2D data: angles in the image plane (film side-on)";
    else if (detectedForward_ == 0) note = "Forward axis not detected — check Engine › Forward axis";
    else if (facingFlips3D_) note = "Facing flips mid-session — tracker may swap left/right";
    dataNoteLabel_->setVisible(!note.isEmpty());
    dataNoteLabel_->setText(note);
    dataNoteLabel_->setProperty("dsStatusPill", "warning");
    WidgetKit::repolish(dataNoteLabel_);
    durationLabel_->setText("/ " + fmtTimecode(dur));
    transport_->setEnabled(n >= 2);

    timeline_->setFrameTimes(times_);
    timeline_->setFlags(session_.flags());

    autoDetectRecordedAvatar();
    syncEngineCombo();
    syncInspector();

    cur_ = 0;
    playTime_ = 0.0;
    setCameraPreset("front");
    recompute();
    return true;
}

void BiofeedbackDevTab::autoDetectRecordedAvatar() {
    const QFileInfo info(sessionPath_);
    QString prefix;
    QDateTime start;
    if (!parseSessionName(info.fileName(), ".csv", prefix, start)) return;
    const double dur = times_.empty() ? 0.0 : times_.back();

    QString bestPath;
    double bestDelta = std::numeric_limits<double>::max();
    const QStringList files = QDir(info.absolutePath()).entryList({"*_biofeedback_avatar.csv"}, QDir::Files);
    for (const QString& f : files) {
        QString p;
        QDateTime when;
        if (!parseSessionName(f, "_biofeedback_avatar.csv", p, when) || p != prefix) continue;
        const double delta = start.secsTo(when);
        if (delta < -2.0 || delta > dur + 5.0) continue;  // biofeedback started during this recording
        if (std::fabs(delta) < std::fabs(bestDelta)) { bestDelta = delta; bestPath = info.absolutePath() + "/" + f; }
    }
    if (!bestPath.isEmpty()) loadRecordedAvatar(bestPath, bestDelta);
}

bool BiofeedbackDevTab::loadRecordedAvatar(const QString& path, double offsetGuess) {
    PlaybackManager pm;
    if (!pm.loadCSV(path.toStdString()) || pm.frameCount() == 0) {
        avatarStatus_->setText("Could not load avatar");
        return false;
    }
    avatar_ = std::move(pm);
    avatarPath_ = path;
    avatarLoaded_ = true;
    double errMm = 0.0;
    avatarOffset_ = alignRecordedAvatar(offsetGuess, &errMm);
    const QString name = QFileInfo(path).fileName();
    const QString offset = QString("%1%2 s").arg(avatarOffset_ >= 0 ? "+" : "").arg(avatarOffset_, 0, 'f', 2);
    if (errMm < 0) {
        avatarStatus_->setText(QString("%1 — could not align: no overlapping upper-body joints").arg(name));
        avatarStatus_->setStyleSheet("color:#ff6767;");
    } else if (errMm > 15.0) {  // correct alignments match to ~2 mm
        avatarStatus_->setText(QString("%1 — best match at %2 is poor (%3 mm): different session or camera?")
                                   .arg(name, offset).arg(errMm, 0, 'f', 0));
        avatarStatus_->setStyleSheet("color:#f2b84b;");
    } else {
        avatarStatus_->setText(QString("%1 — aligned at %2 (match %3 mm)").arg(name, offset).arg(errMm, 0, 'f', 1));
        avatarStatus_->setStyleSheet("");
    }
    avatarStatus_->setToolTip(path);
    syncEngineCombo();
    scheduleRecompute();
    return true;
}

// The avatar is the smoothed live frame with only the legs modified (and,
// with the anatomical engine, the whole body shifted), so its trunk posture —
// head / neck / chest / navel relative to the pelvis — matches the raw
// recording: find the time offset where it lines up best. Midline joints only,
// so a left/right swap in either recording doesn't matter.
double BiofeedbackDevTab::alignRecordedAvatar(double guess, double* matchErrorMm) {
    if (!avatarLoaded_ || times_.empty()) return 0.0;
    static const int refJoints[] = {(int)CanonicalJoint::HEAD, (int)CanonicalJoint::NECK,
                                    (int)CanonicalJoint::SPINE_CHEST, (int)CanonicalJoint::SPINE_NAVEL};
    const int pelvis = (int)CanonicalJoint::PELVIS;
    struct Sample { double t; TrackedBody body; };
    std::vector<Sample> samples;
    const int an = avatar_.frameCount();
    const int stride = std::max(1, an / 60);
    for (int i = 0; i < an && samples.size() < 60; i += stride) {
        const PlaybackFrame* f = avatar_.getFrame(i);
        if (f->bodies.empty()) continue;
        const TrackedBody* b = &f->bodies.front();
        for (const auto& c : f->bodies) if (c.id == primaryBodyId_) b = &c;
        samples.push_back({f->timeSeconds, *b});
    }
    const bool hasGuess = std::isfinite(guess);
    if (samples.empty()) {
        if (matchErrorMm) *matchErrorMm = -1.0;
        return hasGuess ? guess : 0.0;
    }

    auto errorAt = [&](double offset) {
        double sum = 0.0;
        int count = 0;
        for (const Sample& s : samples) {
            const int r = frameIndexAt(s.t + offset);
            if (r < 0 || !hasBody_[r] || std::fabs(times_[r] - (s.t + offset)) > 0.1) continue;
            const TrackedBody& rb = raw_[r];
            if (s.body.joints[pelvis].confidence < 0.1f || rb.joints[pelvis].confidence < 0.1f) continue;
            if (!finitePos(s.body.joints[pelvis]) || !finitePos(rb.joints[pelvis])) continue;
            for (int j : refJoints) {
                if (s.body.joints[j].confidence < 0.1f || rb.joints[j].confidence < 0.1f) continue;
                if (!finitePos(s.body.joints[j]) || !finitePos(rb.joints[j])) continue;
                const bf::Vec3 a = jointPos(s.body, j) - jointPos(s.body, pelvis);
                const bf::Vec3 b = jointPos(rb, j) - jointPos(rb, pelvis);
                sum += bf::length(a - b);
                ++count;
            }
        }
        return count >= 8 ? sum / count : std::numeric_limits<double>::max();
    };

    // candidate offsets put the middle sample on each raw frame in turn, so
    // biofeedback that started before the recording (negative offset) works too
    const double anchor = samples[samples.size() / 2].t;
    double bestOffset = hasGuess ? guess : 0.0, bestErr = std::numeric_limits<double>::max();
    for (size_t r = 0; r < times_.size(); ++r) {
        const double offset = times_[r] - anchor;
        if (hasGuess && std::fabs(offset - guess) > 3.0) continue;
        const double e = errorAt(offset);
        if (e < bestErr) { bestErr = e; bestOffset = offset; }
    }
    if (matchErrorMm) *matchErrorMm = bestErr < 1e9 ? bestErr * 1000.0 : -1.0;
    return bestOffset;
}

bool BiofeedbackDevTab::recordedBodyAt(int frame, TrackedBody& out) const {
    if (!avatarLoaded_ || frame < 0 || frame >= (int)times_.size()) return false;
    const double t = times_[frame] - avatarOffset_;
    if (t < -0.02 || t > avatar_.duration() + 0.02) return false;
    const PlaybackFrame* f = avatar_.getFrameAtTime(std::max(0.0, t));
    if (!f || f->bodies.empty() || std::fabs(f->timeSeconds - t) > 0.1) return false;
    out = f->bodies.front();
    for (const auto& b : f->bodies)
        if (b.id == primaryBodyId_) out = b;
    // the avatar was recorded from the tracker's labels: undo the same swap
    if (frame < (int)swapped_.size() && swapped_[frame]) anat::swapLeftRight(out);
    return true;
}

// =============================================================================
// Processing
// =============================================================================

void BiofeedbackDevTab::rebuildInput() {
    input_ = raw_;
    gapsFilled_ = 0;
    if (fillGaps_ && !input_.empty()) {
        std::vector<TrackedBody*> ptrs(input_.size(), nullptr);
        for (size_t i = 0; i < input_.size(); ++i)
            if (hasBody_[i]) ptrs[i] = &input_[i];
        gapsFilled_ = anat::fillTrackingGaps(ptrs, times_, 0.25);
    }
    const float s = smoothing_;
    if (s <= 0.0f || input_.empty()) return;
    TrackedBody prev;
    bool havePrev = false;
    for (size_t i = 0; i < input_.size(); ++i) {
        if (!hasBody_[i]) continue;
        TrackedBody& b = input_[i];
        if (!havePrev) { prev = b; havePrev = true; continue; }
        for (int j = 0; j < JOINT_COUNT; ++j) {
            Joint& c = b.joints[j];
            if (c.confidence < 0.1f || !finitePos(c)) continue;
            if (prev.joints[j].confidence < 0.1f || !finitePos(prev.joints[j])) { prev.joints[j] = c; continue; }
            c.x = s * prev.joints[j].x + (1.0f - s) * c.x;
            c.y = s * prev.joints[j].y + (1.0f - s) * c.y;
            c.z = s * prev.joints[j].z + (1.0f - s) * c.z;
            prev.joints[j] = c;
        }
    }
}

void BiofeedbackDevTab::updateForwardSign() {
    int sign = detectedForward_ != 0 ? detectedForward_ : 1;
    if (forwardMode_ == 1) sign = 1;
    else if (forwardMode_ == 2) sign = -1;
    settings_.forwardSign = sign;

    // Per-frame signs: 2D data can turn around (walking back and forth past a
    // side-on camera); in 3D a sustained disagreement means the tracker likely
    // swapped left/right (subject facing away) and is only reported.
    const int n = (int)input_.size();
    std::vector<const TrackedBody*> ptrs(n, nullptr);
    for (int i = 0; i < n; ++i)
        if (hasBody_[i]) ptrs[i] = &input_[i];
    const double dur = times_.empty() ? 0.0 : times_.back();
    const int window = std::max(9, (int)std::lround(n > 1 && dur > 0 ? (n - 1) / dur : 30.0));  // ~1 s
    const std::vector<int> perFrame = anat::forwardSignsPerFrame(ptrs, sign, window);
    int turns = 0;
    for (int i = 1; i < n; ++i) turns += perFrame[i] != perFrame[i - 1];
    facingFlips3D_ = false;
    if (planar_ && forwardMode_ == 0) {
        frameForward_ = perFrame;
    } else {
        frameForward_.assign(n, sign);
        if (!planar_) {
            double runStart = -1.0;
            for (int i = 0; i < n && !facingFlips3D_; ++i) {
                const bool against = hasBody_[i] && perFrame[i] != sign;
                if (against && runStart < 0) runStart = times_[i];
                if (!against) runStart = -1.0;
                if (runStart >= 0 && times_[i] - runStart >= 2.0) facingFlips3D_ = true;
            }
        }
    }
    if (forwardCombo_) {
        QString autoText;
        if (planar_ && turns > 0) autoText = QString("Auto (per frame: subject turns %1x)").arg(turns);
        else if (detectedForward_ == 0) autoText = "Auto (not detected, using +)";
        else autoText = QString("Auto (detected %1)").arg(detectedForward_ > 0 ? "+" : "-");
        forwardCombo_->setItemText(0, autoText);
    }
    // Left-handed data (anterior = ml × up, e.g. Kinect after its Y flip) is
    // mirrored for display so the subject's left leg is drawn on their left
    const bool mirror = !planar_ && sign > 0;
    viewOriginal_->setMirrorZ(mirror);
    viewModified_->setMirrorZ(mirror);

    // Session-average body axes along the floor, for the camera presets
    const bf::Vec3 up = settings_.floorUp;
    bf::Vec3 ant, ml;
    const int stride = std::max(1, n / 500);
    for (int i = 0; i < n; i += stride) {
        if (!hasBody_[i]) continue;
        const anat::BodyAxes ax = anat::computeAxes(input_[i], settingsAt(i));
        if (!ax.valid) continue;
        ant += ax.ant;
        ml += ax.ml;
    }
    sessionAnt_ = bf::normalizeOr(bf::rejectFrom(ant, up), bf::Vec3(0, 0, -1));
    sessionMl_ = bf::normalizeOr(bf::rejectFrom(ml, up), bf::Vec3(-1, 0, 0));
}

void BiofeedbackDevTab::applyFloorMode() {
    floor_ = (floorMode_ == 0 && floorFitted_.valid) ? floorFitted_ : floorLevel_;
    settings_.floorUp = floor_.valid ? floor_.up : bf::Vec3(0, 1, 0);
    if (floorCombo_) {
        QString autoText = "Auto";
        if (floorFitted_.valid)
            autoText = floorFitted_.tiltDeg >= 0.5f ? QString("Auto (fitted: %1° tilt)").arg(floorFitted_.tiltDeg, 0, 'f', 1)
                                                    : QString("Auto (level)");
        floorCombo_->setItemText(0, autoText);
    }
}

anat::Settings BiofeedbackDevTab::settingsAt(int i) const {
    anat::Settings s = settings_;
    if (i >= 0 && i < (int)frameForward_.size()) s.forwardSign = frameForward_[i];
    s.maxVerticalSpeed = smoothVertical_ ? 0.5f : 0.0f;
    s.plantStanceFoot = locomotionMode_ == 2 || (locomotionMode_ == 0 && detectedOverground_);
    return s;
}

// Overground when the pelvis travels > 1.2 m within some 4 s window (the same
// rule the live processor uses).
void BiofeedbackDevTab::detectLocomotion() {
    detectedOverground_ = false;
    const bf::Vec3 up = floor_.valid ? floor_.up : bf::Vec3(0, 1, 0);
    std::vector<std::pair<double, bf::Vec3>> track;
    for (size_t i = 0; i < raw_.size(); ++i) {
        const Joint& p = raw_[i].joints[(int)CanonicalJoint::PELVIS];
        if (!hasBody_[i] || p.confidence < 0.1f || !finitePos(p)) continue;
        track.emplace_back(times_[i], bf::rejectFrom(bf::Vec3(p.x, p.y, p.z), up));
    }
    size_t a = 0;
    for (size_t b = 0; b < track.size() && !detectedOverground_; ++b) {
        while (track[b].first - track[a].first > 4.0) ++a;
        if (bf::length(track[b].second - track[a].second) > 1.2f) detectedOverground_ = true;
    }
    if (locomotionCombo_)
        locomotionCombo_->setItemText(0, detectedOverground_ ? "Auto (overground: planting)" : "Auto (in place)");
}

void BiofeedbackDevTab::recomputeNow() {
    recomputeTimer_->stop();
    recompute();
}

void BiofeedbackDevTab::scheduleRecompute() {
    recomputeTimer_->start();
}

anat::LiveProcessor::Options BiofeedbackDevTab::liveOptions() const {
    anat::LiveProcessor::Options o;
    o.fillGaps = fillGaps_;
    o.fixLeftRightSwaps = fixSwaps_;
    o.learnFloor = floorMode_ == 0;
    o.locomotion = (anat::LiveProcessor::Locomotion)locomotionMode_;
    return o;
}

void BiofeedbackDevTab::pushLiveProfile() {
    if (!ctrl_) return;
    anat::Settings s = settings_;
    s.maxVerticalSpeed = smoothVertical_ ? 0.5f : 0.0f;
    ctrl_->setAnatomicalProfile(s, liveOptions());
}

void BiofeedbackDevTab::recompute() {
    if (useLiveCb_ && useLiveCb_->isChecked()) pushLiveProfile();
    const int n = (int)input_.size();
    liveSeen_.clear();
    output_.assign(n, TrackedBody{});
    outputMissing_.assign(n, 0);
    diag_.assign(n, anat::BodyDiagnostics{});

    // Map the same gains onto the existing engine for A/B comparison
    legacy_.clearTransforms();
    if (engine_ == Engine::Legacy) {
        static const BiomechAngle hip[2] = {BiomechAngle::LEFT_HIP_FLEXION, BiomechAngle::RIGHT_HIP_FLEXION};
        static const BiomechAngle knee[2] = {BiomechAngle::LEFT_KNEE_FLEXION, BiomechAngle::RIGHT_KNEE_FLEXION};
        static const BiomechAngle ankle[2] = {BiomechAngle::LEFT_ANKLE_DORSIFLEXION,
                                              BiomechAngle::RIGHT_ANKLE_DORSIFLEXION};
        for (int s = 0; s < 2; ++s) {
            const anat::LimbSettings& ls = settings_.limbs[s];
            if (!ls.enabled || ls.mode != anat::LimbMode::JointAngles) continue;
            if (ls.hipGain != 1.0f) legacy_.setTransform(hip[s], ls.hipGain, ls.hipGain, 0, 0);
            if (ls.kneeGain != 1.0f) legacy_.setTransform(knee[s], ls.kneeGain, ls.kneeGain, 0, 0);
            if (ls.ankleGain != 1.0f) legacy_.setTransform(ankle[s], ls.ankleGain, ls.ankleGain, 0, 0);
        }
    }

    anat::TemporalState state;  // frames run in order, like a live session
    for (int i = 0; i < n; ++i) {
        if (!hasBody_[i]) { output_[i] = input_[i]; continue; }
        const anat::Settings fs = settingsAt(i);
        switch (engine_) {
            case Engine::Anatomical:
                output_[i] = anat::apply(input_[i], fs, &diag_[i], &state, times_[i]);
                break;
            case Engine::Legacy: {
                FrameData f;
                f.bodies.push_back(input_[i]);
                const FrameData o = legacy_.applyTransforms(f, 0.0f);
                output_[i] = o.bodies.empty() ? input_[i] : o.bodies.front();
                diag_[i] = anat::compare(input_[i], output_[i], fs);
                break;
            }
            case Engine::Recorded: {
                TrackedBody rb;
                if (recordedBodyAt(i, rb)) {
                    output_[i] = rb;
                    diag_[i] = anat::compare(input_[i], output_[i], fs);
                } else {
                    output_[i] = input_[i];
                    outputMissing_[i] = 1;
                }
                break;
            }
            case Engine::LivePreview:
                break;  // below: needs its own pass over the uncorrected input
        }
    }

    if (engine_ == Engine::LivePreview) {
        // Exactly the live path: the recording as captured (no look-ahead gap
        // filling or swap fixing), causal smoothing, then the LiveProcessor.
        anat::LiveProcessor live;
        anat::LiveProcessor::Options o = liveOptions();
        o.forwardHint = detectedForward_;  // live knows the camera type; offline this is the equivalent
        live.setOptions(o);
        liveSeen_.assign(n, TrackedBody{});
        TrackedBody prev;
        bool havePrev = false;
        const float sm = smoothing_;
        for (int i = 0; i < n; ++i) {
            if (!hasBody_[i]) { output_[i] = input_[i]; continue; }
            TrackedBody b = rawOriginal_[i];
            if (sm > 0.0f) {
                if (havePrev) {
                    for (int j = 0; j < JOINT_COUNT; ++j) {
                        Joint& c = b.joints[j];
                        if (c.confidence < 0.1f || !finitePos(c)) continue;
                        if (prev.joints[j].confidence < 0.1f || !finitePos(prev.joints[j])) continue;
                        c.x = sm * prev.joints[j].x + (1.0f - sm) * c.x;
                        c.y = sm * prev.joints[j].y + (1.0f - sm) * c.y;
                        c.z = sm * prev.joints[j].z + (1.0f - sm) * c.z;
                    }
                }
                prev = b;
                havePrev = true;
            }
            anat::Settings fs = settings_;
            fs.maxVerticalSpeed = smoothVertical_ ? 0.5f : 0.0f;
            output_[i] = live.process(b, times_[i], fs, &diag_[i]);
            liveSeen_[i] = live.lastInput();
        }
    }
    rebuildTracks();
    refreshFrame();
}

void BiofeedbackDevTab::rebuildTracks() {
    const int n = (int)diag_.size();
    const int s = selectedLimb_;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    auto series = [&](const QString& name, const QColor& color, bool dashed, auto fn) {
        BiofeedbackTimeline::Series out;
        out.name = name;
        out.color = color;
        out.dashed = dashed;
        out.values.resize(n, nan);
        for (int i = 0; i < n; ++i) {
            const anat::BodyDiagnostics& d = diag_[i];
            if (!d.valid || !d.limbs[s].in.valid || outputMissing_[i]) continue;
            out.values[i] = fn(d.limbs[s], d);
        }
        return out;
    };
    auto deg = [](float r) { return r * kRadToDeg; };
    auto ankleIn = [](const anat::LimbDiagnostics& l) {
        return l.in.ankleValid ? l.in.ankle : std::numeric_limits<float>::quiet_NaN();
    };
    auto ankleOut = [](const anat::LimbDiagnostics& l) {
        return l.out.ankleValid ? l.out.ankle : std::numeric_limits<float>::quiet_NaN();
    };

    std::vector<BiofeedbackTimeline::Track> tracks;
    const bool step = settings_.limbs[s].mode == anat::LimbMode::StepLength;
    // The recorded avatar came from a live session: there is no target to show
    const bool targets = engine_ != Engine::Recorded;
    auto tgt = [targets](float v) { return targets ? v : std::numeric_limits<float>::quiet_NaN(); };
    tracks.push_back({"Hip flexion", "°", {
        series("In", kInputColor, false, [&](auto& l, auto&) { return deg(l.in.hip); }),
        series("Tgt", kTargetColor, true, [&](auto& l, auto&) { return tgt(deg(l.target[0])); }),
        series("Out", kOutputColor, false, [&](auto& l, auto&) { return l.out.valid ? deg(l.out.hip) : NAN; })}});
    tracks.push_back({"Knee flexion", "°", {
        series("In", kInputColor, false, [&](auto& l, auto&) { return deg(l.in.knee); }),
        series("Tgt", kTargetColor, true, [&](auto& l, auto&) { return tgt(deg(l.target[1])); }),
        series("Out", kOutputColor, false, [&](auto& l, auto&) { return l.out.valid ? deg(l.out.knee) : NAN; })}});
    tracks.push_back({"Ankle dorsiflex.", "°", {
        series("In", kInputColor, false, [&](auto& l, auto&) { return deg(ankleIn(l)); }),
        series("Tgt", kTargetColor, true, [&](auto& l, auto&) { return tgt(deg(l.target[2])); }),
        series("Out", kOutputColor, false, [&](auto& l, auto&) { return deg(ankleOut(l)); })}});
    tracks.push_back({"Ankle ahead of hip", "mm", {
        series("In", kInputColor, false, [&](auto& l, auto&) { return l.in.stepAP * 1000.0f; }),
        series("Tgt", kTargetColor, true, [&](auto& l, auto&) { return tgt(step ? l.targetStepAP * 1000.0f : NAN); }),
        series("Out", kOutputColor, false, [&](auto& l, auto&) { return l.out.valid ? l.out.stepAP * 1000.0f : NAN; })}});
    tracks.push_back({"Checks", "mm", {
        series("Off", QColor(255, 140, 90), false, [&](auto& l, auto&) { return l.valid ? l.offPlane * 1000.0f : NAN; }),
        series("Bone", QColor(255, 103, 103), false, [&](auto& l, auto&) { return l.valid ? l.boneLengthError * 1000.0f : NAN; }),
        series("Gnd", QColor(120, 170, 255), false, [&](auto&, auto& d) { return d.groundShift * 1000.0f; })}});
    for (auto& tr : tracks) {
        tr.minSpan = tr.unit == "mm" ? 10.0f : 5.0f;
        tr.includeZero = tr.name == "Checks";
    }
    timeline_->setTracks(std::move(tracks));
}

// =============================================================================
// Display
// =============================================================================

void BiofeedbackDevTab::refreshFrame() {
    BiofeedbackDevView::Scene a, b;
    a.title = "ORIGINAL";
    a.accent = kOriginalAccent;
    b.title = "MODIFIED";
    b.accent = kOutputColor;
    static const char* engineNames[] = {"Anatomical engine (new)", "Current live engine", "Recorded avatar",
                                        "Live preview (frame by frame)"};
    b.subtitle = engineNames[(int)engine_];
    if (engine_ == Engine::Legacy) {
        for (const auto& ls : settings_.limbs)
            if (ls.enabled && ls.mode == anat::LimbMode::StepLength) {
                b.subtitle += " · has no step-length mode: step limbs unchanged";
                break;
            }
    }
    a.subtitle = smoothing_ > 0.0f ? QString("Recorded joints · smoothing %1").arg(smoothing_, 0, 'f', 2)
                                   : QString("Recorded joints");
    a.emptyText = b.emptyText = times_.empty() ? "Open a recorded session to begin" : "Subject not tracked in this frame";

    const int i = cur_;
    const bool valid = i >= 0 && i < (int)input_.size() && hasBody_[i];
    if (valid) {
        // live preview: show what the live processor saw (its own gap holding / swap fixing)
        const bool live = engine_ == Engine::LivePreview && i < (int)liveSeen_.size();
        auto inBody = std::make_shared<TrackedBody>(live ? liveSeen_[i] : input_[i]);
        a.body = inBody;
        a.hasFloor = b.hasFloor = floor_.valid;
        a.floorUp = b.floorUp = floor_.up;
        a.floorHeight = b.floorHeight = floor_.height;
        a.showAxes = b.showAxes = showAxes_;
        const anat::Settings fs = settingsAt(i);
        a.axes = anat::computeAxes(input_[i], fs);

        if (i < (int)output_.size() && !outputMissing_[i]) {
            b.body = std::make_shared<TrackedBody>(output_[i]);
            b.axes = anat::computeAxes(output_[i], fs);
            if (showGhost_) b.ghost = inBody;
            // planted stance foot: the avatar travels further than the person; follow it
            if (i < (int)diag_.size()) b.targetOffset = diag_[i].rootOffset;
        } else {
            b.emptyText = "No recorded biofeedback at this time";
        }

        // Highlight the limbs being modified
        for (int s = 0; s < 2; ++s) {
            if (!settings_.limbs[s].enabled) continue;
            const auto& L = anat::legJoints((anat::Side)s);
            for (int j : {L.hip, L.knee, L.ankle, L.foot}) b.highlight[j] = true;
        }

        if (showLabels_ && i < (int)diag_.size() && diag_[i].valid) {
            const anat::LimbDiagnostics& ld = diag_[i].limbs[selectedLimb_];
            const auto& L = anat::legJoints((anat::Side)selectedLimb_);
            if (ld.in.valid) {
                const QColor inC(220, 230, 238);
                a.labels.push_back({L.hip, fmtDeg(ld.in.hip), inC});
                a.labels.push_back({L.knee, fmtDeg(ld.in.knee), inC});
                if (ld.in.ankleValid) a.labels.push_back({L.ankle, fmtDeg(ld.in.ankle), inC});
            }
            if (ld.out.valid && b.body) {
                b.labels.push_back({L.hip, fmtDeg(ld.out.hip), kOutputColor});
                b.labels.push_back({L.knee, fmtDeg(ld.out.knee), kOutputColor});
                if (ld.out.ankleValid) b.labels.push_back({L.ankle, fmtDeg(ld.out.ankle), kOutputColor});
            }
        }

        // Camera follows the ORIGINAL pelvis, so both views frame identically
        // and any difference on the right is the engine's doing
        // (a briefly untracked pelvis keeps the last position instead of jumping)
        bf::Vec3 target = sessionCentre_;
        if (follow_) {
            const Joint& pel = input_[i].joints[(int)CanonicalJoint::PELVIS];
            if (pel.confidence >= 0.1f && finitePos(pel)) {
                target = jointPos(input_[i], (int)CanonicalJoint::PELVIS);
                lastTarget_ = target;
                haveLastTarget_ = true;
            } else if (haveLastTarget_) {
                target = lastTarget_;
            }
        }
        camA_->target = QVector3D(target.x, target.y, target.z);
        camA_->up = QVector3D(floor_.up.x, floor_.up.y, floor_.up.z);
        if (camB_ != camA_) {
            camB_->target = camA_->target;
            camB_->up = camA_->up;
        }
    }

    viewOriginal_->setScene(std::move(a));
    viewModified_->setScene(std::move(b));

    if (!times_.empty()) {
        const double t = times_[std::clamp(i, 0, (int)times_.size() - 1)];
        timeline_->setCurrentTime(t);
        timecodeLabel_->setText(fmtTimecode(t));
        frameLabel_->setText(QString("Frame %1 / %2").arg(i + 1).arg(times_.size()));
    } else {
        frameLabel_->clear();
    }
    refreshReadout();
}

void BiofeedbackDevTab::refreshReadout() {
    const int i = cur_;
    const int s = selectedLimb_;
    QString html = QString("<div style='color:#f2b84b; font-weight:650;'>%1 leg</div>").arg(anat::sideName((anat::Side)s));
    if (i < 0 || i >= (int)diag_.size() || !diag_[i].valid || !diag_[i].limbs[s].in.valid) {
        html += "<div style='color:#9da8b5;'>No measurement for this limb at this frame.</div>";
        readout_->setText(html);
        return;
    }
    const anat::BodyDiagnostics& d = diag_[i];
    const anat::LimbDiagnostics& l = d.limbs[s];
    const bool hasOut = l.out.valid && !outputMissing_[i];
    auto cell = [](const QString& v, const char* color = "#ecf1f7") {
        return QString("<td align='right' style='color:%1; padding-left:10px;'>%2</td>").arg(color, v);
    };
    html += "<table cellspacing='0' cellpadding='1'>"
            "<tr><td></td><td align='right' style='color:#9da8b5; padding-left:10px;'>Input</td>"
            "<td align='right' style='color:#82b9ff; padding-left:10px;'>Target</td>"
            "<td align='right' style='color:#f2b84b; padding-left:10px;'>Output</td></tr>";
    auto row = [&](const QString& name, float in, float target, float out) {
        html += "<tr><td style='color:#9da8b5;'>" + name + "</td>" + cell(fmtDeg(in)) +
                cell(fmtDeg(target), "#82b9ff") + cell(hasOut ? fmtDeg(out) : "—", "#f2b84b") + "</tr>";
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const bool targets = engine_ != Engine::Recorded;
    row("Hip flexion", l.in.hip, targets ? l.target[0] : nan, l.out.hip);
    row("Knee flexion", l.in.knee, targets ? l.target[1] : nan, l.out.knee);
    row("Ankle dorsiflex.", l.in.ankleValid ? l.in.ankle : nan, targets ? l.target[2] : nan,
        l.out.ankleValid ? l.out.ankle : nan);
    const bool step = targets && settings_.limbs[s].enabled && settings_.limbs[s].mode == anat::LimbMode::StepLength;
    html += "<tr><td style='color:#9da8b5;'>Ankle ahead of hip</td>" + cell(fmtMm(l.in.stepAP)) +
            cell(step ? fmtMm(l.targetStepAP) : "—", "#82b9ff") + cell(hasOut ? fmtMm(l.out.stepAP) : "—", "#f2b84b") +
            "</tr></table>";
    if (hasOut) {
        html += QString("<div style='margin-top:6px;' title='Movement out of each joint&#39;s own flexion plane: "
                        "0 for any pure flexion change'>Off-plane <b style='color:%1'>%2</b>"
                        " &nbsp; Bone length <b style='color:%3'>%4</b></div>")
                    .arg(errorColor(l.offPlane), fmtMm(l.offPlane))
                    .arg(errorColor(l.boneLengthError), fmtMm(l.boneLengthError));
        html += QString("<div style='color:#9da8b5;'>Ankle sideways %1 &nbsp; Ground shift %2 &nbsp; Pelvis drop %3</div>")
                    .arg(fmtMm(l.lateralShift), fmtMm(d.groundShift), fmtMm(d.pelvisDrop));
        if (d.stanceSide >= 0 || bf::length(d.rootOffset) > 0.0005f)
            html += QString("<div style='color:#9da8b5;'>Planted: %1 foot &nbsp; Avatar ahead by %2</div>")
                        .arg(d.stanceSide == 0 ? "left" : d.stanceSide == 1 ? "right" : "—", fmtMm(bf::length(d.rootOffset)));
    }
    readout_->setText(html);
}

void BiofeedbackDevTab::syncInspector() {
    const int s = selectedLimb_;
    const anat::LimbSettings& ls = settings_.limbs[s];
    inspectorTitle_->setText(QString("%1 leg").arg(anat::sideName((anat::Side)s)));
    {
        QSignalBlocker b1(modeAnglesRb_), b2(modeStepRb_);
        modeAnglesRb_->setChecked(ls.mode == anat::LimbMode::JointAngles);
        modeStepRb_->setChecked(ls.mode == anat::LimbMode::StepLength);
    }
    auto setRow = [](GainRow& r, float v) {
        QSignalBlocker b1(r.slider), b2(r.spin);
        r.slider->setValue((int)std::lround(v * 100.0f));
        r.spin->setValue(v);
    };
    setRow(hipRow_, ls.hipGain);
    setRow(kneeRow_, ls.kneeGain);
    setRow(ankleRow_, ls.ankleGain);
    setRow(stepRow_, ls.stepGain);
    const bool step = ls.mode == anat::LimbMode::StepLength;
    hipRow_.row->setVisible(!step);
    kneeRow_.row->setVisible(!step);
    stepRow_.row->setVisible(step);

    const anat::Neutral& n = settings_.neutral[s];
    neutralLabel_->setText(QString("Neutral  hip %1 · knee %2 · ankle %3 · step %4")
                               .arg(fmtDeg(n.hip), fmtDeg(n.knee), fmtDeg(n.ankle), fmtMm(n.stepAP)));
}

void BiofeedbackDevTab::syncLimbList() {
    QSignalBlocker block(limbList_);
    for (int s = 0; s < 2; ++s) {
        const anat::LimbSettings& ls = settings_.limbs[s];
        QString summary;
        if (ls.mode == anat::LimbMode::StepLength)
            summary = QString("Step x%1").arg(ls.stepGain, 0, 'f', 2);
        else
            summary = QString("Hip x%1  Knee x%2").arg(ls.hipGain, 0, 'f', 2).arg(ls.kneeGain, 0, 'f', 2);
        if (std::fabs(ls.ankleGain - 1.0f) > 1e-4f) summary += QString("  Ankle x%1").arg(ls.ankleGain, 0, 'f', 2);
        QListWidgetItem* item = limbList_->item(s);
        item->setText(QString("%1 leg\n%2").arg(anat::sideName((anat::Side)s), summary));
        item->setCheckState(ls.enabled ? Qt::Checked : Qt::Unchecked);
        item->setForeground(ls.enabled ? QColor(236, 241, 247) : QColor(120, 132, 147));
    }
}

void BiofeedbackDevTab::syncEngineCombo() {
    if (auto* model = qobject_cast<QStandardItemModel*>(engineCombo_->model())) {
        if (QStandardItem* item = model->item((int)Engine::Recorded)) item->setEnabled(avatarLoaded_);
    }
    if (!avatarLoaded_) {
        avatarStatus_->setText("No recorded avatar for this session");
        if (engine_ == Engine::Recorded) setEngine(Engine::Anatomical);
    }
}

// =============================================================================
// Programmatic control
// =============================================================================

void BiofeedbackDevTab::setSettings(const anat::Settings& settings) {
    const int fwd = settings_.forwardSign;
    const bf::Vec3 floorUp = settings_.floorUp;
    settings_ = settings;
    settings_.forwardSign = fwd;     // owned by the Forward axis control
    settings_.floorUp = floorUp;     // fitted from the session
    {
        QSignalBlocker b1(groundCb_), b2(pelvisDropCb_), b3(hipRefCombo_);
        groundCb_->setChecked(settings_.groundContact);
        pelvisDropCb_->setChecked(settings_.pelvisDrop);
        hipRefCombo_->setCurrentIndex((int)settings_.hipReference);
    }
    syncLimbList();
    syncInspector();
    scheduleRecompute();
}

void BiofeedbackDevTab::setEngine(Engine engine) {
    engineCombo_->setCurrentIndex((int)engine);  // signals → engine_ + recompute
    engine_ = engine;
}

void BiofeedbackDevTab::setSelectedLimb(int side) {
    limbList_->setCurrentRow(std::clamp(side, 0, 1));
}

void BiofeedbackDevTab::seekToTime(double seconds) {
    playTime_ = seconds;
    setCurrentFrame(frameIndexAt(seconds));
}

void BiofeedbackDevTab::setCameraPreset(const QString& preset) {
    BfOrbitCamera& c = *camA_;
    c.up = QVector3D(floor_.up.x, floor_.up.y, floor_.up.z);
    c.pan = QVector3D();
    c.distance = 2.9f;
    c.pitch = 10.0f;
    if (planar_) {
        // 2D data lives in the image plane: always look at it from the camera
        c.yaw = viewOriginal_->yawToSee(bf::Vec3(0, 0, 1));
        c.pitch = preset == "top" ? 82.0f : 4.0f;
    } else if (preset == "left") c.yaw = viewOriginal_->yawToSee(-sessionMl_);
    else if (preset == "right") c.yaw = viewOriginal_->yawToSee(sessionMl_);
    else if (preset == "top") { c.yaw = viewOriginal_->yawToSee(sessionAnt_); c.pitch = 82.0f; c.distance = 3.4f; }
    else c.yaw = viewOriginal_->yawToSee(sessionAnt_);
    if (camB_ != camA_) *camB_ = c;
    viewOriginal_->refresh();
    viewModified_->refresh();
}

// =============================================================================
// Playback
// =============================================================================

int BiofeedbackDevTab::frameIndexAt(double t) const {
    if (times_.empty()) return 0;
    auto it = std::upper_bound(times_.begin(), times_.end(), t);
    if (it == times_.begin()) return 0;
    return (int)std::distance(times_.begin(), it) - 1;
}

void BiofeedbackDevTab::setCurrentFrame(int index) {
    if (times_.empty()) return;
    index = std::clamp(index, 0, (int)times_.size() - 1);
    if (!playing_) playTime_ = times_[index];
    cur_ = index;
    refreshFrame();
}

void BiofeedbackDevTab::stepFrames(int delta) {
    setPlaying(false);
    setCurrentFrame(cur_ + delta);
}

void BiofeedbackDevTab::setPlaying(bool playing) {
    if (times_.size() < 2) playing = false;
    playing_ = playing;
    playBtn_->setIcon(transportIcon(playing ? "pause" : "play"));
    if (playing) {
        if (cur_ >= (int)times_.size() - 1) { cur_ = 0; playTime_ = 0.0; }
        lastTickNs_ = clock_.nsecsElapsed();
        playTimer_->start();
    } else {
        playTimer_->stop();
    }
}

void BiofeedbackDevTab::onTick() {
    const qint64 now = clock_.nsecsElapsed();
    const double dt = (now - lastTickNs_) / 1e9;
    lastTickNs_ = now;
    playTime_ += dt * speed_;
    const double dur = times_.empty() ? 0.0 : times_.back();
    if (playTime_ > dur) {
        if (loop_) playTime_ = 0.0;
        else { playTime_ = dur; setPlaying(false); }
    }
    const int idx = frameIndexAt(playTime_);
    if (idx != cur_) {
        cur_ = idx;
        refreshFrame();
    }
}

// =============================================================================
// Keyboard / visibility / files
// =============================================================================

void BiofeedbackDevTab::keyPressEvent(QKeyEvent* e) {
    // Only reaches here when the focused child didn't use the key, so a focused
    // slider or spin box keeps its arrows.
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    switch (e->key()) {
        case Qt::Key_Space: setPlaying(!playing_); break;
        case Qt::Key_Left: stepFrames(shift ? -10 : -1); break;
        case Qt::Key_Right: stepFrames(shift ? 10 : 1); break;
        case Qt::Key_Home: setPlaying(false); setCurrentFrame(0); break;
        case Qt::Key_End: setPlaying(false); setCurrentFrame((int)times_.size() - 1); break;
        default: QWidget::keyPressEvent(e); return;
    }
    e->accept();
}

void BiofeedbackDevTab::hideEvent(QHideEvent* e) {
    setPlaying(false);  // don't keep animating behind another tab
    QWidget::hideEvent(e);
}

QString BiofeedbackDevTab::resolveSessionPath(const QString& picked) {
    const QFileInfo fi(picked);
    const QString name = fi.fileName();
    const QString dir = fi.absolutePath();
    static const char* companions[] = {"_flags.csv", "_biofeedback_avatar.csv", "_biofeedback_angles.csv",
                                       "_metadata.json"};
    for (const char* sfx : companions) {
        if (!name.endsWith(sfx)) continue;
        const QString stem = name.left(name.size() - (int)std::strlen(sfx));
        if (QFile::exists(dir + "/" + stem + ".csv")) return dir + "/" + stem + ".csv";
        QString prefix;
        QDateTime when;
        if (!parseSessionName(stem + ".csv", ".csv", prefix, when)) break;
        const QString cam1 = dir + "/" + prefix + "_cam1_" + when.toString("yyyyMMdd_HHmmss") + ".csv";
        if (QFile::exists(cam1)) return cam1;
        // Biofeedback files are stamped when biofeedback STARTED: use the
        // recording with the same prefix that began most recently before it
        QString bestPath;
        qint64 bestDelta = LLONG_MAX;
        for (const QString& f : QDir(dir).entryList({prefix + "_*.csv"}, QDir::Files)) {
            QString p2;
            QDateTime w2;
            if (!parseSessionName(f, ".csv", p2, w2) || p2 != prefix) continue;
            const qint64 d = w2.secsTo(when);
            if (d >= -2 && d < bestDelta) { bestDelta = d; bestPath = dir + "/" + f; }
        }
        if (!bestPath.isEmpty()) return bestPath;
        break;
    }
    return picked;
}
