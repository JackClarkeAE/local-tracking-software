#include "biofeedback_dev_tab.h"
#include "biofeedback_dev_view.h"
#include "biofeedback_timeline.h"
#include "widget_kit.h"
#include "../app_controller.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
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
#include <QShortcut>
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
#include <cmath>
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
    return QString::number(rad * kRadToDeg, 'f', 1) + "°";
}

QString fmtMm(float m) {
    if (!std::isfinite(m)) return "—";
    return QString::number(m * 1000.0f, 'f', 0) + " mm";
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
    connect(timeline_, &BiofeedbackTimeline::seekRequested, this, [this](double t) {
        playTime_ = t;
        setCurrentFrame(frameIndexAt(t));
    });
    timeLayout->addWidget(timeline_, 1);
    vsplit->addWidget(timeFrame);

    // --- Development controls
    vsplit->addWidget(buildControls());

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

    auto shortcut = [this](QKeySequence key, auto fn) {
        auto* s = new QShortcut(key, this);
        s->setContext(Qt::WidgetWithChildrenShortcut);
        connect(s, &QShortcut::activated, this, fn);
    };
    shortcut(QKeySequence(Qt::Key_Space), [this] { setPlaying(!playing_); });
    shortcut(QKeySequence(Qt::Key_Left), [this] { stepFrames(-1); });
    shortcut(QKeySequence(Qt::Key_Right), [this] { stepFrames(1); });
    shortcut(QKeySequence(Qt::SHIFT | Qt::Key_Left), [this] { stepFrames(-10); });
    shortcut(QKeySequence(Qt::SHIFT | Qt::Key_Right), [this] { stepFrames(10); });
    shortcut(QKeySequence(Qt::Key_Home), [this] { setCurrentFrame(0); });
    shortcut(QKeySequence(Qt::Key_End), [this] { setCurrentFrame((int)times_.size() - 1); });

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

    auto* hint = new QLabel("Timeline: click/drag to scrub · wheel to zoom · Shift+wheel to scroll · double-click ruler to fit");
    hint->setProperty("muted", true);
    QFont hf = hint->font();
    hf.setPointSizeF(7.5);
    hint->setFont(hf);
    l->addWidget(hint);
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
    name->setMinimumWidth(118);
    l->addWidget(name);
    r.slider = new QSlider(Qt::Horizontal);
    r.slider->setRange(0, 300);
    r.slider->setPageStep(10);
    r.slider->setToolTip(tip);
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
    limbList_->setMinimumWidth(190);
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
    engineCombo_->setToolTip("Which engine produces the MODIFIED view.\n"
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
        scheduleRecompute();
    });
    grid->addWidget(smoothingSpin_, 0, 1);

    grid->addWidget(new QLabel("Hip reference:"), 1, 0);
    hipRefCombo_ = new QComboBox;
    hipRefCombo_->addItem("Pelvis (anatomical)");
    hipRefCombo_->addItem("World vertical");
    hipRefCombo_->setToolTip("Measure hip flexion against the pelvis (pelvis → spine) or against vertical.\n"
                             "World vertical ignores trunk lean.");
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
    eng->addLayout(grid);

    groundCb_ = new QCheckBox("Ground contact (keep lowest foot point at its height)");
    groundCb_->setChecked(settings_.groundContact);
    connect(groundCb_, &QCheckBox::toggled, this, [this](bool on) {
        settings_.groundContact = on;
        scheduleRecompute();
    });
    eng->addWidget(groundCb_);
    pelvisDropCb_ = new QCheckBox("Step mode: lower pelvis when a step is out of reach");
    pelvisDropCb_->setChecked(settings_.pelvisDrop);
    connect(pelvisDropCb_, &QCheckBox::toggled, this, [this](bool on) {
        settings_.pelvisDrop = on;
        scheduleRecompute();
    });
    eng->addWidget(pelvisDropCb_);

    auto* displayRow = new QHBoxLayout;
    ghostCb_ = new QCheckBox("Ghost original");
    ghostCb_->setChecked(showGhost_);
    axesCb_ = new QCheckBox("Body axes");
    axesCb_->setChecked(showAxes_);
    labelsCb_ = new QCheckBox("Angle labels");
    labelsCb_->setChecked(showLabels_);
    connect(ghostCb_, &QCheckBox::toggled, this, [this](bool on) { showGhost_ = on; refreshFrame(); });
    connect(axesCb_, &QCheckBox::toggled, this, [this](bool on) { showAxes_ = on; refreshFrame(); });
    connect(labelsCb_, &QCheckBox::toggled, this, [this](bool on) { showLabels_ = on; refreshFrame(); });
    displayRow->addWidget(ghostCb_);
    displayRow->addWidget(axesCb_);
    displayRow->addWidget(labelsCb_);
    displayRow->addStretch();
    eng->addLayout(displayRow);
    eng->addStretch();

    // --- Readout
    auto* ro = column("This frame", 3);
    readout_ = new QLabel;
    readout_->setTextFormat(Qt::RichText);
    readout_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    readout_->setWordWrap(true);
    readout_->setMinimumWidth(240);
    ro->addWidget(readout_, 1);
    return panel;
}

// =============================================================================
// Session loading
// =============================================================================

bool BiofeedbackDevTab::loadSession(const QString& path) {
    setPlaying(false);
    PlaybackManager pm;
    if (!pm.loadCSV(path.toStdString()) || pm.frameCount() == 0) {
        QMessageBox::warning(this, "Biofeedback Development", "Could not load joint data from:\n" + path);
        return false;
    }
    session_ = std::move(pm);
    sessionPath_ = path;

    // The body seen in the most frames is the subject
    std::map<uint32_t, int> counts;
    for (int i = 0; i < session_.frameCount(); ++i)
        for (const auto& b : session_.getFrame(i)->bodies) counts[b.id]++;
    primaryBodyId_ = 0;
    int best = -1;
    for (auto [id, n] : counts)
        if (n > best) { best = n; primaryBodyId_ = id; }

    const int n = session_.frameCount();
    times_.assign(n, 0.0);
    raw_.assign(n, TrackedBody{});
    hasBody_.assign(n, 0);
    for (int i = 0; i < n; ++i) {
        const PlaybackFrame* f = session_.getFrame(i);
        times_[i] = f->timeSeconds;
        for (const auto& b : f->bodies) {
            if (b.id == primaryBodyId_) { raw_[i] = b; hasBody_[i] = 1; break; }
        }
    }

    // Floor (2nd percentile of the lowest foot point), centre, 2D detection
    std::vector<float> lows;
    double cx = 0, cy = 0, cz = 0;
    int centreCount = 0;
    int planarVotes = 0, bodyCount = 0;
    for (int i = 0; i < n; ++i) {
        if (!hasBody_[i]) continue;
        const TrackedBody& b = raw_[i];
        float low = std::numeric_limits<float>::max();
        for (int j : {20, 21, 24, 25})
            if (b.joints[j].confidence >= 0.1f) low = std::min(low, b.joints[j].y);
        if (low < std::numeric_limits<float>::max()) lows.push_back(low);
        const Joint& pel = b.joints[(int)CanonicalJoint::PELVIS];
        if (pel.confidence >= 0.1f) { cx += pel.x; cy += pel.y; cz += pel.z; ++centreCount; }
        bool flat = true;
        for (const auto& j : b.joints)
            if (j.confidence >= 0.1f && std::fabs(j.z) > 1e-5f) { flat = false; break; }
        planarVotes += flat ? 1 : 0;
        ++bodyCount;
    }
    hasFloor_ = !lows.empty();
    if (hasFloor_) {
        std::sort(lows.begin(), lows.end());
        floorY_ = lows[(size_t)(lows.size() * 0.02)];
    }
    sessionCentre_ = centreCount ? bf::Vec3((float)(cx / centreCount), (float)(cy / centreCount), (float)(cz / centreCount))
                                 : bf::Vec3();
    planar_ = bodyCount > 0 && planarVotes * 2 > bodyCount;

    // Forward axis from where the feet point (sampled)
    std::vector<const TrackedBody*> sample;
    const int stride = std::max(1, n / 3000);
    for (int i = 0; i < n; i += stride)
        if (hasBody_[i]) sample.push_back(&raw_[i]);
    detectedForward_ = anat::detectForwardSign(sample);

    settings_.neutral = {};
    avatarLoaded_ = false;
    avatar_.clear();
    avatarPath_.clear();
    rebuildInput();
    updateForwardSign();

    // Session info
    const double dur = times_.empty() ? 0.0 : times_.back();
    const double fps = dur > 0 ? (n - 1) / dur : 0.0;
    sessionLabel_->setText(QString("%1   ·   %2   ·   %3 frames   ·   %4 fps   ·   %5")
                               .arg(QFileInfo(path).fileName(), fmtDuration(dur))
                               .arg(n)
                               .arg(fps, 0, 'f', 1)
                               .arg(planar_ ? "2D (RGB)" : "3D"));
    sessionLabel_->setToolTip(path);
    dataNoteLabel_->setVisible(planar_ || detectedForward_ == 0);
    if (planar_) {
        dataNoteLabel_->setText("2D data: angles in the image plane (film side-on)");
        dataNoteLabel_->setProperty("dsStatusPill", "warning");
    } else if (detectedForward_ == 0) {
        dataNoteLabel_->setText("Forward axis not detected — check Engine › Forward axis");
        dataNoteLabel_->setProperty("dsStatusPill", "warning");
    }
    WidgetKit::repolish(dataNoteLabel_);
    durationLabel_->setText("/ " + fmtTimecode(dur));

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
        if (delta < bestDelta) { bestDelta = delta; bestPath = info.absolutePath() + "/" + f; }
    }
    if (!bestPath.isEmpty()) loadRecordedAvatar(bestPath, std::max(0.0, bestDelta));
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
    avatarStatus_->setText(QString("%1 — aligned at +%2 s (match %3 mm)")
                               .arg(QFileInfo(path).fileName())
                               .arg(avatarOffset_, 0, 'f', 2)
                               .arg(errMm, 0, 'f', 1));
    avatarStatus_->setToolTip(path);
    syncEngineCombo();
    scheduleRecompute();
    return true;
}

// The avatar is the smoothed live frame with only the legs modified, so its
// head / chest / shoulders match the raw recording: find the time offset
// where they line up best.
double BiofeedbackDevTab::alignRecordedAvatar(double guess, double* matchErrorMm) {
    if (!avatarLoaded_ || times_.empty()) return 0.0;
    static const int refJoints[] = {(int)CanonicalJoint::HEAD, (int)CanonicalJoint::NECK,
                                    (int)CanonicalJoint::SPINE_CHEST, (int)CanonicalJoint::SHOULDER_LEFT,
                                    (int)CanonicalJoint::SHOULDER_RIGHT};
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
    if (samples.empty()) return std::max(0.0, guess);

    auto errorAt = [&](double offset) {
        double sum = 0.0;
        int count = 0;
        for (const Sample& s : samples) {
            const int r = frameIndexAt(s.t + offset);
            if (r < 0 || !hasBody_[r] || std::fabs(times_[r] - (s.t + offset)) > 0.1) continue;
            for (int j : refJoints) {
                if (s.body.joints[j].confidence < 0.1f || raw_[r].joints[j].confidence < 0.1f) continue;
                sum += bf::length(jointPos(s.body, j) - jointPos(raw_[r], j));
                ++count;
            }
        }
        return count >= 10 ? sum / count : std::numeric_limits<double>::max();
    };

    const double first = samples.front().t;
    double bestOffset = std::max(0.0, guess), bestErr = std::numeric_limits<double>::max();
    for (size_t r = 0; r < times_.size(); ++r) {
        const double offset = times_[r] - first;
        if (offset < -0.5) continue;
        if (guess >= 0.0 && std::fabs(offset - guess) > 3.0) continue;
        const double e = errorAt(offset);
        if (e < bestErr) { bestErr = e; bestOffset = offset; }
    }
    if (matchErrorMm) *matchErrorMm = bestErr < 1e9 ? bestErr * 1000.0 : -1.0;
    return bestOffset;
}

const TrackedBody* BiofeedbackDevTab::recordedBodyAt(double rawTime) const {
    if (!avatarLoaded_) return nullptr;
    const double t = rawTime - avatarOffset_;
    if (t < -0.02 || t > avatar_.duration() + 0.02) return nullptr;
    const PlaybackFrame* f = avatar_.getFrameAtTime(std::max(0.0, t));
    if (!f || f->bodies.empty() || std::fabs(f->timeSeconds - t) > 0.1) return nullptr;
    for (const auto& b : f->bodies) if (b.id == primaryBodyId_) return &b;
    return &f->bodies.front();
}

// =============================================================================
// Processing
// =============================================================================

void BiofeedbackDevTab::rebuildInput() {
    input_ = raw_;
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
            if (c.confidence < 0.1f) continue;
            if (prev.joints[j].confidence < 0.1f) { prev.joints[j] = c; continue; }
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
    if (forwardCombo_) {
        const QString autoText = detectedForward_ == 0 ? QString("Auto (not detected, using +)")
                                                       : QString("Auto (detected %1)").arg(detectedForward_ > 0 ? "+" : "-");
        forwardCombo_->setItemText(0, autoText);
    }
    // Left-handed data (anterior = ml × up, e.g. Kinect after its Y flip) is
    // mirrored for display so the subject's left leg is drawn on their left
    const bool mirror = !planar_ && sign > 0;
    viewOriginal_->setMirrorZ(mirror);
    viewModified_->setMirrorZ(mirror);

    // Session-average body axes, for the camera presets
    bf::Vec3 ant, ml;
    const int stride = std::max(1, (int)input_.size() / 500);
    for (size_t i = 0; i < input_.size(); i += stride) {
        if (!hasBody_[i]) continue;
        const anat::BodyAxes ax = anat::computeAxes(input_[i], settings_);
        if (!ax.valid) continue;
        ant += ax.ant;
        ml += ax.ml;
    }
    sessionAnt_ = bf::normalizeOr(bf::Vec3(ant.x, 0, ant.z), bf::Vec3(0, 0, -1));
    sessionMl_ = bf::normalizeOr(bf::Vec3(ml.x, 0, ml.z), bf::Vec3(-1, 0, 0));
}

void BiofeedbackDevTab::scheduleRecompute() {
    recomputeTimer_->start();
}

void BiofeedbackDevTab::recompute() {
    const int n = (int)input_.size();
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

    for (int i = 0; i < n; ++i) {
        if (!hasBody_[i]) { output_[i] = input_[i]; continue; }
        switch (engine_) {
            case Engine::Anatomical:
                output_[i] = anat::apply(input_[i], settings_, &diag_[i]);
                break;
            case Engine::Legacy: {
                FrameData f;
                f.bodies.push_back(input_[i]);
                const FrameData o = legacy_.applyTransforms(f, 0.0f);
                output_[i] = o.bodies.empty() ? input_[i] : o.bodies.front();
                diag_[i] = anat::compare(input_[i], output_[i], settings_);
                break;
            }
            case Engine::Recorded: {
                const TrackedBody* rb = recordedBodyAt(times_[i]);
                if (rb) {
                    output_[i] = *rb;
                    diag_[i] = anat::compare(input_[i], output_[i], settings_);
                } else {
                    output_[i] = input_[i];
                    outputMissing_[i] = 1;
                }
                break;
            }
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
        series("Side", QColor(255, 140, 90), false, [&](auto& l, auto&) { return l.valid ? l.lateralShift * 1000.0f : NAN; }),
        series("Bone", QColor(255, 103, 103), false, [&](auto& l, auto&) { return l.valid ? l.boneLengthError * 1000.0f : NAN; }),
        series("Gnd", QColor(120, 170, 255), false, [&](auto&, auto& d) { return d.groundShift * 1000.0f; })}});
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
    static const char* engineNames[] = {"Anatomical engine (new)", "Current live engine", "Recorded avatar"};
    b.subtitle = engineNames[(int)engine_];
    a.subtitle = smoothing_ > 0.0f ? QString("Recorded joints · smoothing %1").arg(smoothing_, 0, 'f', 2)
                                   : QString("Recorded joints");
    a.emptyText = b.emptyText = times_.empty() ? "Open a recorded session to begin" : "Subject not tracked in this frame";

    const int i = cur_;
    const bool valid = i >= 0 && i < (int)input_.size() && hasBody_[i];
    if (valid) {
        auto inBody = std::make_shared<TrackedBody>(input_[i]);
        a.body = inBody;
        a.hasFloor = b.hasFloor = hasFloor_;
        a.floorY = b.floorY = floorY_;
        a.showAxes = b.showAxes = showAxes_;
        a.axes = anat::computeAxes(input_[i], settings_);

        if (i < (int)output_.size() && !outputMissing_[i]) {
            b.body = std::make_shared<TrackedBody>(output_[i]);
            b.axes = anat::computeAxes(output_[i], settings_);
            if (showGhost_) b.ghost = inBody;
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
        bf::Vec3 target = sessionCentre_;
        if (follow_) {
            const Joint& pel = input_[i].joints[(int)CanonicalJoint::PELVIS];
            if (pel.confidence >= 0.1f) target = jointPos(input_[i], (int)CanonicalJoint::PELVIS);
        }
        camA_->target = QVector3D(target.x, target.y, target.z);
        if (camB_ != camA_) camB_->target = camA_->target;
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
        html += QString("<div style='margin-top:6px;'>Sideways drift <b style='color:%1'>%2</b>"
                        " &nbsp; Bone length <b style='color:%3'>%4</b></div>")
                    .arg(errorColor(l.lateralShift), fmtMm(l.lateralShift))
                    .arg(errorColor(l.boneLengthError), fmtMm(l.boneLengthError));
        html += QString("<div style='color:#9da8b5;'>Ground shift %1 &nbsp; Pelvis drop %2</div>")
                    .arg(fmtMm(d.groundShift), fmtMm(d.pelvisDrop));
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
    settings_ = settings;
    settings_.forwardSign = fwd;  // forward axis is owned by the Forward axis control
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
    auto yawOf = [this](const bf::Vec3& dataDir) {
        const QVector3D d = viewOriginal_->toDisplay(dataDir);
        return qRadiansToDegrees(std::atan2(d.x(), d.z()));
    };
    c.pan = QVector3D();
    c.distance = 2.9f;
    c.pitch = 10.0f;
    if (planar_) {
        // 2D data lives in the image plane: always look at it from the camera
        c.yaw = 0.0f;
        c.pitch = preset == "top" ? 82.0f : 4.0f;
    } else if (preset == "left") c.yaw = yawOf(-sessionMl_);
    else if (preset == "right") c.yaw = yawOf(sessionMl_);
    else if (preset == "top") { c.yaw = yawOf(sessionAnt_); c.pitch = 82.0f; c.distance = 3.4f; }
    else c.yaw = yawOf(sessionAnt_);
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
    if (times_.empty()) playing = false;
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
