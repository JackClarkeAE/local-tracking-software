#include "clinical_record_tab.h"
#include "skeleton_view.h"
#include "popout_window.h"
#include "camera_feed_view.h"
#include "widget_kit.h"
#include "../camera/model_tracker.h"
#include "../app_controller.h"
#include "../config.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSplitter>
#include <QLineEdit>
#include <QRegularExpression>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QTimer>
#include <QProgressBar>
#include <QFrame>
#include <QScrollArea>
#include <QMessageBox>
#include <QMediaDevices>
#include <QCameraDevice>
#include <algorithm>
#include <mutex>
#include <set>

namespace {

QWidget* labeledRow(QVBoxLayout* layout, const QString& labelText, QWidget* field,
                    int labelWidth = 96) {
    auto* row = new QWidget;
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);

    auto* label = new QLabel(labelText);
    label->setProperty("rowLabel", true);
    label->setFixedWidth(labelWidth);
    rowLayout->addWidget(label);
    rowLayout->addWidget(field, 1);
    layout->addWidget(row);
    return row;
}

QWidget* buttonRow(QVBoxLayout* layout, QWidget* left, QWidget* right) {
    auto* row = new QWidget;
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);
    rowLayout->addWidget(left, 1);
    rowLayout->addWidget(right, 1);
    layout->addWidget(row);
    return row;
}

QWidget* viewPanel(const QString& title, QWidget* view) {
    auto* panel = WidgetKit::panel(title);
    panel->setProperty("livePanel", true);
    static_cast<QVBoxLayout*>(panel->layout())->addWidget(view, 1);
    return panel;
}

QString shortParameter(const std::string& parameter) {
    QString text = QString::fromStdString(parameter);
    const int pipe = text.indexOf('|');
    if (pipe >= 0) text = text.left(pipe);
    if (text.length() > 50) text = text.left(50) + "…";
    return text;
}

QString mmss(float seconds) {
    const int total = static_cast<int>(seconds);
    return QString("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QChar('0'));
}

}

ClinicalRecordTab::ClinicalRecordTab(AppController* ctrl, QWidget* parent)
    : QWidget(parent), ctrl_(ctrl) {
    // Clinical sessions always capture joint data; there is no toggle for it.
    ctrl_->recordJoints = true;

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setHandleWidth(6);
    root->addWidget(splitter);

    // --- Views -------------------------------------------------------------
    patientScreen_ = new SkeletonView;
    patientScreen_->setMode(SkeletonView::Mode::AutoFit2D);
    patientScreenPanel_ = viewPanel("Patient Screen", patientScreen_);
    splitter->addWidget(patientScreenPanel_);

    clinicianView_ = new SkeletonView;
    clinicianView_->setMode(SkeletonView::Mode::AutoFit2D);
    clinicianViewPanel_ = viewPanel("Clinician View", clinicianView_);
    recordingBanner_ = new QLabel("●  RECORDING");
    recordingBanner_->setAlignment(Qt::AlignCenter);
    recordingBanner_->setProperty("recordingBanner", true);
    recordingBanner_->setVisible(false);
    static_cast<QVBoxLayout*>(clinicianViewPanel_->layout())->insertWidget(0, recordingBanner_);
    splitter->addWidget(clinicianViewPanel_);

    feedView_ = new CameraFeedView;
    feedPanel_ = viewPanel("Camera View", feedView_);
    feedPanel_->setVisible(false);
    splitter->addWidget(feedPanel_);

    // --- Controls ----------------------------------------------------------
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* controls = WidgetKit::panel("SESSION");
    controls->setProperty("controlsPanel", true);
    scroll->setWidget(controls);
    auto* cl = static_cast<QVBoxLayout*>(controls->layout());
    cl->setContentsMargins(12, 10, 12, 12);
    cl->setSpacing(6);

    statusPill_ = WidgetKit::statusPill();
    statusPill_->setAlignment(Qt::AlignCenter);
    cl->addWidget(statusPill_);

    // Camera
    WidgetKit::addSectionHeader(cl, "Camera");
    cameraCategoryCombo_ = new QComboBox;
    cameraCategoryCombo_->addItems({"Depth camera (skeleton)", "RGB camera"});
    cameraCategoryCombo_->setToolTip(
        "Depth camera: ZED or Azure Kinect with built-in skeleton tracking\n"
        "RGB camera: any webcam / USB camera, tracked by a pose model");
    connect(cameraCategoryCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ClinicalRecordTab::onCameraCategoryChanged);
    labeledRow(cl, "Type", cameraCategoryCombo_);

    cameraDeviceCombo_ = new QComboBox;
    connect(cameraDeviceCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ClinicalRecordTab::onCameraSelectionChanged);
    labeledRow(cl, "Device", cameraDeviceCombo_);

    rgbModelCombo_ = new QComboBox;
    rgbModelCombo_->setToolTip("Pose model used to track the patient on an RGB camera.");
    connect(rgbModelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ClinicalRecordTab::onCameraSelectionChanged);
    rgbModelRow_ = labeledRow(cl, "Tracking", rgbModelCombo_);
    rgbModelRow_->setVisible(false);

    startCamBtn_ = WidgetKit::button("Start Camera", WidgetKit::ButtonRole::Primary);
    stopCamBtn_ = WidgetKit::button("Stop Camera", WidgetKit::ButtonRole::Destructive);
    connect(startCamBtn_, &QPushButton::clicked, this, &ClinicalRecordTab::onStartCamera);
    connect(stopCamBtn_, &QPushButton::clicked, this, &ClinicalRecordTab::onStopCamera);
    buttonRow(cl, startCamBtn_, stopCamBtn_);

    // Assessment
    WidgetKit::addSectionHeader(cl, "Assessment");
    protocolCombo_ = new QComboBox;
    labeledRow(cl, "Protocol", protocolCombo_);
    runProtocolBtn_ = WidgetKit::button("Run Assessment", WidgetKit::ButtonRole::Primary);
    stopProtocolBtn_ = WidgetKit::button("Stop Assessment", WidgetKit::ButtonRole::Destructive);
    connect(runProtocolBtn_, &QPushButton::clicked, this, &ClinicalRecordTab::onRunAssessment);
    connect(stopProtocolBtn_, &QPushButton::clicked, this, &ClinicalRecordTab::onStopAssessment);
    buttonRow(cl, runProtocolBtn_, stopProtocolBtn_);

    protocolInfoLabel_ = new QLabel;
    protocolInfoLabel_->setWordWrap(true);
    protocolInfoLabel_->setProperty("muted", true);
    cl->addWidget(protocolInfoLabel_);

    protocolProgress_ = new QProgressBar;
    protocolProgress_->setMaximumHeight(12);
    protocolProgress_->setTextVisible(false);
    protocolProgress_->setVisible(false);
    cl->addWidget(protocolProgress_);

    currentBox_ = new QFrame;
    currentBox_->setProperty("eventBox", "current");
    auto* curLayout = new QVBoxLayout(currentBox_);
    curLayout->setContentsMargins(8, 6, 8, 6);
    currentEventLabel_ = new QLabel;
    currentEventLabel_->setWordWrap(true);
    curLayout->addWidget(currentEventLabel_);
    currentBox_->setVisible(false);
    cl->addWidget(currentBox_);

    countdownLabel_ = new QLabel;
    countdownLabel_->setAlignment(Qt::AlignCenter);
    countdownLabel_->setProperty("countdown", true);
    countdownLabel_->setVisible(false);
    cl->addWidget(countdownLabel_);

    nextBox_ = new QFrame;
    nextBox_->setProperty("eventBox", "next");
    auto* nextLayout = new QVBoxLayout(nextBox_);
    nextLayout->setContentsMargins(8, 6, 8, 6);
    nextEventLabel_ = new QLabel;
    nextEventLabel_->setWordWrap(true);
    nextLayout->addWidget(nextEventLabel_);
    nextBox_->setVisible(false);
    cl->addWidget(nextBox_);

    // Patient
    WidgetKit::addSectionHeader(cl, "Patient");
    patientIdEdit_ = new QLineEdit;
    patientIdEdit_->setPlaceholderText("Required to run an assessment");
    clinicianEdit_ = new QLineEdit;
    clinicianEdit_->setPlaceholderText("Name or initials");
    notesEdit_ = new QPlainTextEdit;
    notesEdit_->setPlaceholderText("Optional");
    // Three lines tall by default to encourage clinicians to write notes.
    notesEdit_->setFixedHeight(3 * patientIdEdit_->sizeHint().height());
    notesEdit_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    connect(patientIdEdit_, &QLineEdit::textChanged, this, [this](const QString& s) {
        ctrl_->patientId = s.toStdString();
        updateButtons();
    });
    connect(clinicianEdit_, &QLineEdit::textChanged, this, [this](const QString& s) {
        ctrl_->operatorName = s.toStdString();
    });
    connect(notesEdit_, &QPlainTextEdit::textChanged, this, [this] {
        ctrl_->sessionNotes = notesEdit_->toPlainText().toStdString();
    });
    labeledRow(cl, "Patient ID", patientIdEdit_);
    labeledRow(cl, "Clinician", clinicianEdit_);
    auto* notesRow = labeledRow(cl, "Notes", notesEdit_);
    // The row must not soak up the panel's spare height; pin it to the box.
    notesRow->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    if (auto* notesLabel = notesRow->findChild<QLabel*>())
        notesRow->layout()->setAlignment(notesLabel, Qt::AlignTop);

    // Patient screen
    WidgetKit::addSectionHeader(cl, "Patient Screen");
    patientScreenCb_ = new QCheckBox("Show patient screen in this window");
    patientScreenCb_->setChecked(true);
    connect(patientScreenCb_, &QCheckBox::toggled, this, [this](bool) { updateViewLayout(); });
    cl->addWidget(patientScreenCb_);
    popOutBtn_ = WidgetKit::button("Open Patient Screen on Second Display");
    popOutBtn_->setToolTip("Opens the patient screen as its own window. Press F11 in that window for full screen.");
    connect(popOutBtn_, &QPushButton::clicked, this, &ClinicalRecordTab::onPopOut);
    cl->addWidget(popOutBtn_);

    cl->addStretch();

    sessionInfoLabel_ = new QLabel;
    sessionInfoLabel_->setProperty("muted", true);
    cl->addWidget(sessionInfoLabel_);

    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 38);
    splitter->setStretchFactor(1, 38);
    splitter->setStretchFactor(2, 38);
    splitter->setStretchFactor(3, 24);
    splitter->setSizes({700, 700, 700, 420});

    // Controller signals
    connect(ctrl_, &AppController::sessionStateChanged, this, &ClinicalRecordTab::onSessionStateChanged);
    connect(ctrl_, &AppController::protocolActionFired, this, [this](QString) { updateProtocolUI(); });
    connect(ctrl_, &AppController::protocolsChanged, this, [this] { refreshProtocolList(); });
    connect(ctrl_, &AppController::overlayChanged, this, [this] {
        patientScreen_->setOverlayState(ctrl_->overlayState());
        if (popout_) popout_->view()->setOverlayState(ctrl_->overlayState());
    });

    tickTimer_ = new QTimer(this);
    tickTimer_->setInterval(33);
    connect(tickTimer_, &QTimer::timeout, this, &ClinicalRecordTab::onTick);
    tickTimer_->start();

    populateDeviceCombo();
    applyCameraSettings();
    refreshProtocolList();
    applyDefaults();
    updateViewLayout();
    updateButtons();
    updateStatus();
    updateProtocolUI();
}

void ClinicalRecordTab::applyDefaults() {
    if (ctrl_->sessionState() != SessionState::Stopped) return;
    const AppConfig& cfg = ctrl_->config();

    const int category = cfg.defaultCameraType == "rgb" ? 1 : 0;
    if (cameraCategoryCombo_->currentIndex() != category)
        cameraCategoryCombo_->setCurrentIndex(category);  // repopulates device/model combos
    else
        onCameraCategoryChanged();

    auto select = [](QComboBox* combo, const std::string& text) {
        if (text.empty()) return;
        const int idx = combo->findText(QString::fromStdString(text));
        if (idx >= 0) combo->setCurrentIndex(idx);
    };
    select(cameraDeviceCombo_, cfg.defaultDevice);
    if (category == 1) select(rgbModelCombo_, cfg.defaultRgbModel);
    select(protocolCombo_, cfg.defaultProtocol);
    if (clinicianEdit_->text().isEmpty() && !cfg.defaultClinician.empty())
        clinicianEdit_->setText(QString::fromStdString(cfg.defaultClinician));

    applyCameraSettings();
    updateButtons();
    updateProtocolUI();
}

ClinicalRecordTab::~ClinicalRecordTab() {
    if (popout_) { popout_->close(); delete popout_; popout_ = nullptr; }
}

// ---------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------

void ClinicalRecordTab::onTick() {
    std::shared_ptr<FrameData> frame;
    {
        auto& slot = ctrl_->slot(0);
        std::lock_guard<std::mutex> lock(slot.frameMutex);
        frame = slot.sharedFrame;
    }

    if (feedPanel_->isVisible()) feedView_->setFrame(frame);

    if (frame) {
        auto smoothed = std::make_shared<FrameData>(*frame);
        ctrl_->applySmoothingToFrame(*smoothed, 0);

        // Protocol-driven biofeedback still applies to the patient screen so
        // clinical protocols authored in the research UI behave the same.
        std::shared_ptr<FrameData> patientFrame = smoothed;
        if (ctrl_->biofeedbackActive() && ctrl_->biofeedbackEngine().hasActiveTransforms()) {
            const float protoTime = (ctrl_->protocolRunner().state() == ProtocolRunnerState::Running)
                ? ctrl_->protocolRunner().currentTime()
                : ctrl_->debugInfo.sessionSeconds;
            patientFrame = std::make_shared<FrameData>(
                ctrl_->biofeedbackEngine().applyTransforms(*smoothed, protoTime));
        }

        patientScreen_->setAvatarVisible(ctrl_->avatarVisible());
        patientScreen_->setOverlayState(ctrl_->overlayState());
        patientScreen_->setShowJointAngles(ctrl_->showJointAngles);
        patientScreen_->setFrame(patientFrame);

        clinicianView_->clearOverlay();
        clinicianView_->setFrame(smoothed);

        if (popout_ && popout_->isVisible()) {
            popout_->view()->setAvatarVisible(ctrl_->avatarVisible());
            popout_->view()->setOverlayState(ctrl_->overlayState());
            popout_->view()->setShowJointAngles(ctrl_->showJointAngles);
            popout_->view()->setFrame(patientFrame);
        }
    }

    const auto& d = ctrl_->debugInfo;
    QString info = QString("Session %1  ·  %2 FPS  ·  %3 tracked")
        .arg(mmss(d.sessionSeconds))
        .arg(d.cameraFps[0], 0, 'f', 0)
        .arg(d.bodyCount[0]);
    if (ctrl_->protocolRunner().state() == ProtocolRunnerState::Running)
        info += QString("  ·  Assessment %1").arg(mmss(ctrl_->protocolRunner().currentTime()));
    sessionInfoLabel_->setText(info);

    updateProtocolUI();
    updateButtons();
}

// ---------------------------------------------------------------------------
// State → widgets
// ---------------------------------------------------------------------------

void ClinicalRecordTab::updateButtons() {
    const bool stopped = ctrl_->sessionState() == SessionState::Stopped;
    const bool running = ctrl_->sessionState() == SessionState::Running;
    const bool recording = ctrl_->isAnyRecording();
    const bool protoRunning = ctrl_->protocolRunner().state() == ProtocolRunnerState::Running;

    recordingBanner_->setVisible(recording);

    bool cameraAvailable = false;
    if (cameraCategoryCombo_->currentIndex() == 1) {
        cameraAvailable = rgbCamerasPresent_;
    } else {
        cameraAvailable = cameraDeviceCombo_->currentIndex() == 0 ? ctrl_->zedSdkAvailable()
                                                                  : ctrl_->kinectSdkAvailable();
    }

    startCamBtn_->setEnabled(stopped && cameraAvailable && !protoRunning);
    stopCamBtn_->setEnabled(!stopped && !protoRunning);
    cameraCategoryCombo_->setEnabled(stopped);
    cameraDeviceCombo_->setEnabled(stopped);
    rgbModelCombo_->setEnabled(stopped);

    patientIdEdit_->setEnabled(stopped);
    clinicianEdit_->setEnabled(stopped);
    notesEdit_->setEnabled(stopped);

    const bool haveProtocol = protocolCombo_->count() > 0 && !protocolCombo_->currentText().isEmpty();
    runProtocolBtn_->setEnabled(running && !protoRunning && haveProtocol && !ctrl_->patientId.empty());
    stopProtocolBtn_->setEnabled(protoRunning);
    protocolCombo_->setEnabled(!protoRunning);

    // Recording state takes precedence in the status pill, checked every tick
    // because recordings can be started by a protocol, not just the button.
    updateStatus();
}

void ClinicalRecordTab::updateStatus() {
    QString text;
    const char* tone = "neutral";
    if (ctrl_->isAnyRecording()) {
        text = "RECORDING";
        tone = "danger";
    } else {
        switch (ctrl_->sessionState()) {
            case SessionState::Running: text = "CAMERA LIVE"; tone = "success"; break;
            case SessionState::Paused:  text = "PAUSED";      tone = "warning"; break;
            default:                    text = "READY";       tone = "neutral"; break;
        }
    }
    if (statusPill_->text() == text) return;
    statusPill_->setText(text);
    statusPill_->setProperty("dsStatusPill", tone);
    WidgetKit::repolish(statusPill_);
}

void ClinicalRecordTab::updateProtocolUI() {
    auto& runner = ctrl_->protocolRunner();
    const bool running = runner.state() == ProtocolRunnerState::Running;

    if (!running) {
        protocolProgress_->setVisible(false);
        currentBox_->setVisible(false);
        nextBox_->setVisible(false);
        countdownLabel_->setVisible(false);
        // Tell the clinician what still blocks Run Assessment rather than
        // leaving a silently disabled button.
        QString hint;
        if (protocolCombo_->count() == 0 || protocolCombo_->currentText().isEmpty())
            hint = "No protocols found. Add protocol files to the protocols folder.";
        else if (ctrl_->patientId.empty())
            hint = "Enter a Patient ID to run an assessment.";
        else if (ctrl_->sessionState() != SessionState::Running)
            hint = "Start the camera to run an assessment.";
        else if (ctrl_->isProtocolLoaded())
            hint = QString("Ready: %1 (%2 steps)")
                       .arg(QString::fromStdString(ctrl_->loadedProtocol().name))
                       .arg(static_cast<int>(ctrl_->loadedProtocol().events.size()));
        else
            hint = "Press Run Assessment to begin.";
        protocolInfoLabel_->setText(hint);
        return;
    }

    protocolProgress_->setVisible(true);
    const float progress = runner.totalDuration() > 0
        ? runner.currentTime() / runner.totalDuration() : 0.0f;
    protocolProgress_->setValue(static_cast<int>(progress * 100));

    const auto& events = runner.protocol().events;
    const int curIdx = runner.currentEventIndex();
    const int nextIdx = curIdx + 1;
    protocolInfoLabel_->setText(QString("Step %1 of %2").arg(curIdx + 1).arg(runner.totalEvents()));

    if (curIdx >= 0 && curIdx < static_cast<int>(events.size())) {
        const auto& ev = events[curIdx];
        currentEventLabel_->setText(QString("<b>Now [%1]</b><br/>%2<br/><span style='color:#5b6b7b;'>%3</span>")
            .arg(mmss(ev.timeOffsetSeconds))
            .arg(protocolEventTypeName(ev.type))
            .arg(shortParameter(ev.parameter)));
        currentBox_->setVisible(true);
    } else {
        currentBox_->setVisible(false);
    }

    if (nextIdx >= 0 && nextIdx < static_cast<int>(events.size())) {
        const auto& nev = events[nextIdx];
        float countdown = nev.timeOffsetSeconds - runner.currentTime();
        if (countdown < 0) countdown = 0;
        countdownLabel_->setText(countdown >= 60
            ? QString("Next step in %1").arg(mmss(countdown))
            : QString("Next step in %1 s").arg(countdown, 0, 'f', 0));
        countdownLabel_->setVisible(true);
        nextEventLabel_->setText(QString("<b>Next [%1]</b><br/>%2<br/><span style='color:#5b6b7b;'>%3</span>")
            .arg(mmss(nev.timeOffsetSeconds))
            .arg(protocolEventTypeName(nev.type))
            .arg(shortParameter(nev.parameter)));
        nextBox_->setVisible(true);
    } else {
        countdownLabel_->setText("Final step reached.");
        countdownLabel_->setVisible(true);
        nextBox_->setVisible(false);
    }
}

void ClinicalRecordTab::updateViewLayout() {
    const bool rgb = cameraCategoryCombo_->currentIndex() == 1;
    const bool hasModel = rgb && !rgbModelCombo_->currentData().toString().isEmpty();
    feedPanel_->setVisible(rgb);
    // Skeleton views are only meaningful when something produces a skeleton.
    clinicianViewPanel_->setVisible(!rgb || hasModel);
    patientScreenPanel_->setVisible(patientScreenCb_->isChecked() && (!rgb || hasModel));
}

// ---------------------------------------------------------------------------
// Camera selection
// ---------------------------------------------------------------------------

void ClinicalRecordTab::populateDeviceCombo() {
    cameraDeviceCombo_->blockSignals(true);
    cameraDeviceCombo_->clear();
    if (cameraCategoryCombo_->currentIndex() == 1) {
        const auto devices = QMediaDevices::videoInputs();
        rgbCamerasPresent_ = !devices.isEmpty();
        for (const auto& d : devices) cameraDeviceCombo_->addItem(d.description());
        if (devices.isEmpty()) cameraDeviceCombo_->addItem("No RGB cameras detected");
    } else {
        cameraDeviceCombo_->addItems({"ZED 2i", "Azure Kinect"});
        // Prefer whichever depth camera SDK is actually present on this machine.
        if (!ctrl_->zedSdkAvailable() && ctrl_->kinectSdkAvailable())
            cameraDeviceCombo_->setCurrentIndex(1);
    }
    cameraDeviceCombo_->blockSignals(false);
}

void ClinicalRecordTab::populateModelCombo() {
    rgbModelCombo_->blockSignals(true);
    rgbModelCombo_->clear();
    rgbModelCombo_->addItem("None (video only)", QString());
    std::set<std::string> seen;
    auto addFrom = [&](const std::string& dir) {
        for (const auto& m : listRgbModels(dir)) {
            if (!seen.insert(m.name).second) continue;
            rgbModelCombo_->addItem(QString::fromStdString(m.name),
                                    QString::fromStdString(m.onnxPath));
        }
    };
    addFrom(getBundledModelsDir());
    addFrom(ctrl_->config().rgbModelsDir);
    // Default to the first real model so an RGB camera tracks out of the box.
    if (rgbModelCombo_->count() > 1) rgbModelCombo_->setCurrentIndex(1);
    rgbModelCombo_->blockSignals(false);
}

void ClinicalRecordTab::applyCameraSettings() {
    CameraConfig cfg;  // depth-camera tuning stays at the controller defaults
    cfg.deviceIndex = 0;
    if (cameraCategoryCombo_->currentIndex() == 1) {
        ctrl_->setCameraType(0, CameraType::RGBWebcam);
        cfg.rgbDeviceIndex = std::max(0, cameraDeviceCombo_->currentIndex());
        cfg.rgbModelOnnx = rgbModelCombo_->currentData().toString().toStdString();
    } else {
        ctrl_->setCameraType(0, cameraDeviceCombo_->currentIndex() == 0 ? CameraType::ZED2i
                                                                        : CameraType::AzureKinect);
    }
    ctrl_->setCameraConfig(0, cfg);
}

void ClinicalRecordTab::onCameraCategoryChanged() {
    populateDeviceCombo();
    const bool rgb = cameraCategoryCombo_->currentIndex() == 1;
    if (rgb) populateModelCombo();
    rgbModelRow_->setVisible(rgb);
    applyCameraSettings();
    updateViewLayout();
    updateButtons();
}

void ClinicalRecordTab::onCameraSelectionChanged() {
    applyCameraSettings();
    updateViewLayout();
    updateButtons();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void ClinicalRecordTab::onStartCamera() {
    applyCameraSettings();
    ctrl_->startCamera(0);
}

void ClinicalRecordTab::onStopCamera() {
    ctrl_->stopRecording();
    ctrl_->stopCamera(0);
}

void ClinicalRecordTab::onRunAssessment() {
    if (ctrl_->patientId.empty()) {
        QMessageBox::warning(this, "Assessment", "Enter a Patient ID before running an assessment.");
        return;
    }
    const QString name = protocolCombo_->currentText();
    if (name.isEmpty()) return;

    // Load and run in one step: the clinician picks a protocol and presses Run.
    if (ctrl_->isProtocolLoaded()) ctrl_->unloadProtocol();
    Protocol protocol;
    if (!protocol.loadJSON(ctrl_->config().protocolsDir + "/" + name.toStdString())) {
        QMessageBox::warning(this, "Assessment", "Failed to load the selected protocol file.");
        return;
    }
    ctrl_->loadProtocol(protocol);
    ctrl_->recordJoints = true;
    // Recordings are started by the protocol's own events; name them after
    // the patient so the Viewer lists them by ID.
    QString base = QString::fromStdString(ctrl_->patientId).trimmed();
    base.replace(QRegularExpression("[^A-Za-z0-9_-]+"), "_");
    ctrl_->recordingFileName = base.isEmpty() ? "session" : base.toStdString();
    ctrl_->runProtocol();
    updateProtocolUI();
}

void ClinicalRecordTab::onStopAssessment() {
    ctrl_->abortProtocol();
    updateProtocolUI();
}

void ClinicalRecordTab::onPopOut() {
    if (!popout_) popout_ = new PopoutWindow;
    if (popout_->isVisible()) {
        popout_->hide();
        popOutBtn_->setText("Open Patient Screen on Second Display");
    } else {
        popout_->view()->setOverlayState(ctrl_->overlayState());
        popout_->view()->setShowJointAngles(ctrl_->showJointAngles);
        popout_->show();
        popOutBtn_->setText("Close Patient Screen Window");
    }
}

void ClinicalRecordTab::refreshProtocolList() {
    const QString current = protocolCombo_->currentText();
    protocolCombo_->clear();
    for (const auto& f : listProtocolFiles(ctrl_->config().protocolsDir))
        protocolCombo_->addItem(QString::fromStdString(f));
    const int idx = protocolCombo_->findText(current);
    if (idx >= 0) protocolCombo_->setCurrentIndex(idx);
    updateButtons();
}

void ClinicalRecordTab::onSessionStateChanged(int) {
    updateButtons();
}
