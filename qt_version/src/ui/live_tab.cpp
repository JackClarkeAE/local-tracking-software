#include "live_tab.h"
#include "skeleton_view.h"
#include "popout_window.h"
#include "camera_feed_window.h"
#include "camera_feed_view.h"
#include "../camera/model_tracker.h"
#include "../app_controller.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSplitter>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QTimer>
#include <QProgressBar>
#include <QFrame>
#include <QScrollArea>
#include <QMessageBox>
#include <QFileDialog>
#include <QRadioButton>
#include <QStringList>
#include <QMediaDevices>
#include <QCameraDevice>
#include <filesystem>
#include <chrono>
#include <mutex>
#include <algorithm>

static void setPrimary(QObject* object) {
    if (auto* widget = qobject_cast<QWidget*>(object)) {
        widget->setProperty("primary", true);
    }
}

static void setDestructive(QObject* object) {
    if (auto* widget = qobject_cast<QWidget*>(object)) {
        widget->setProperty("destructive", true);
    }
}

static QComboBox* comboWithItems(const QStringList& items) {
    auto* combo = new QComboBox;
    combo->addItems(items);
    return combo;
}

static QFrame* makeLine() {
    auto* line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setProperty("separator", true);
    return line;
}

static void addSectionHeader(QVBoxLayout* layout, const QString& title) {
    auto* row = new QWidget;
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 4, 0, 1);
    rowLayout->setSpacing(7);

    auto* label = new QLabel(title);
    label->setProperty("sectionTitle", true);
    rowLayout->addWidget(label);
    rowLayout->addWidget(makeLine(), 1);
    layout->addWidget(row);
}

static QWidget* addCompactRow(QVBoxLayout* layout, const QString& labelText, QWidget* field,
                              int labelWidth = 86) {
    auto* row = new QWidget;
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);

    rowLayout->addWidget(field, 1);
    auto* label = new QLabel(labelText);
    label->setProperty("rowLabel", true);
    label->setFixedWidth(labelWidth);
    rowLayout->addWidget(label);
    layout->addWidget(row);
    return row;
}

static QWidget* addButtonRow(QVBoxLayout* layout, QWidget* left, QWidget* right = nullptr) {
    auto* row = new QWidget;
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(5);
    rowLayout->addWidget(left, 1);
    if (right) rowLayout->addWidget(right, 1);
    layout->addWidget(row);
    return row;
}

static QPushButton* compactButton(const QString& text) {
    auto* button = new QPushButton(text);
    button->setMinimumHeight(24);
    button->setMaximumHeight(28);
    return button;
}

static QFrame* makePanel() {
    auto* panel = new QFrame;
    panel->setProperty("livePanel", true);
    panel->setFrameShape(QFrame::NoFrame);
    return panel;
}

static QWidget* makeViewPanel(const QString& title, QWidget* view) {
    auto* panel = makePanel();
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(6, 5, 6, 6);
    layout->setSpacing(4);

    auto* label = new QLabel(title);
    label->setProperty("panelTitle", true);
    layout->addWidget(label);
    layout->addWidget(makeLine());
    layout->addWidget(view, 1);
    return panel;
}

LiveTab::LiveTab(AppController* ctrl, QWidget* parent)
    : QWidget(parent), ctrl_(ctrl) {
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setHandleWidth(4);
    root->addWidget(splitter);

    patientScreen_ = new SkeletonView;
    patientScreen_->setMode(SkeletonView::Mode::AutoFit2D);
    patientScreenPanel_ = makeViewPanel("Patient Screen", patientScreen_);
    splitter->addWidget(patientScreenPanel_);

    patientViewer_ = new SkeletonView;
    patientViewer_->setMode(SkeletonView::Mode::AutoFit2D);
    patientViewerPanel_ = makeViewPanel("Patient Viewer", patientViewer_);
    splitter->addWidget(patientViewerPanel_);

    // Inline camera feed — the main view when an RGB camera is selected
    feedView_ = new CameraFeedView;
    feedPanel_ = makeViewPanel("Camera View", feedView_);
    feedPanel_->setVisible(false);
    splitter->addWidget(feedPanel_);

    // Right: compact controls panel. Scroll only becomes relevant at small windows.
    auto* controlsScroll = new QScrollArea;
    controlsScroll->setWidgetResizable(true);
    controlsScroll->setFrameShape(QFrame::NoFrame);
    controlsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    controlsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto* controlsContent = new QFrame;
    controlsContent->setProperty("controlsPanel", true);
    controlsScroll->setWidget(controlsContent);
    auto* cl = new QVBoxLayout(controlsContent);
    cl->setContentsMargins(7, 6, 7, 7);
    cl->setSpacing(4);

    auto* controlsTitle = new QLabel("CONTROLS");
    controlsTitle->setProperty("panelTitle", true);
    cl->addWidget(controlsTitle);
    cl->addWidget(makeLine());

    // Status
    statusLabel_ = new QLabel;
    statusLabel_->setProperty("statusPill", true);
    cl->addWidget(statusLabel_);

    // === Protocol ===
    addSectionHeader(cl, "Protocol");
    auto* protoWidget = new QWidget;
    auto* protoLayout = new QVBoxLayout(protoWidget);
    protoLayout->setContentsMargins(0, 0, 0, 0);
    protoLayout->setSpacing(4);
    protocolCombo_ = new QComboBox;
    protoLayout->addWidget(protocolCombo_);
    loadProtocolBtn_ = compactButton("Load Protocol");
    unloadProtocolBtn_ = compactButton("Unload Protocol");
    runProtocolBtn_ = compactButton("Run Protocol");
    abortProtocolBtn_ = compactButton("Abort Protocol");
    setDestructive(abortProtocolBtn_);
    connect(loadProtocolBtn_, &QPushButton::clicked, this, &LiveTab::onLoadProtocol);
    connect(unloadProtocolBtn_, &QPushButton::clicked, this, &LiveTab::onUnloadProtocol);
    connect(runProtocolBtn_, &QPushButton::clicked, this, &LiveTab::onRunProtocol);
    connect(abortProtocolBtn_, &QPushButton::clicked, this, &LiveTab::onAbortProtocol);
    addButtonRow(protoLayout, loadProtocolBtn_, unloadProtocolBtn_);
    addButtonRow(protoLayout, runProtocolBtn_, abortProtocolBtn_);

    protocolInfoLabel_ = new QLabel;
    protocolInfoLabel_->setWordWrap(true);
    protocolInfoLabel_->setProperty("muted", true);
    protoLayout->addWidget(protocolInfoLabel_);

    protocolProgress_ = new QProgressBar;
    protocolProgress_->setMaximumHeight(14);
    protocolProgress_->setVisible(false);
    protoLayout->addWidget(protocolProgress_);

    currentBox_ = new QFrame;
    currentBox_->setProperty("eventBox", "current");
    auto* curLayout = new QVBoxLayout(currentBox_);
    curLayout->setContentsMargins(6, 4, 6, 4);
    currentEventLabel_ = new QLabel;
    currentEventLabel_->setWordWrap(true);
    curLayout->addWidget(currentEventLabel_);
    currentBox_->setVisible(false);
    protoLayout->addWidget(currentBox_);

    countdownLabel_ = new QLabel;
    countdownLabel_->setAlignment(Qt::AlignCenter);
    countdownLabel_->setProperty("countdown", true);
    countdownLabel_->setVisible(false);
    protoLayout->addWidget(countdownLabel_);

    nextBox_ = new QFrame;
    nextBox_->setProperty("eventBox", "next");
    auto* nxtLayout = new QVBoxLayout(nextBox_);
    nxtLayout->setContentsMargins(6, 4, 6, 4);
    nextEventLabel_ = new QLabel;
    nextEventLabel_->setWordWrap(true);
    nxtLayout->addWidget(nextEventLabel_);
    nextBox_->setVisible(false);
    protoLayout->addWidget(nextBox_);

    cl->addWidget(protoWidget);

    // === Camera ===
    addSectionHeader(cl, "Camera");
    cameraCategoryCombo_ = comboWithItems({"Markerless (Depth)", "RGB Camera", "Markered (TODO)"});
    cameraCategoryCombo_->setToolTip(
        "Markerless: depth cameras with skeleton tracking (ZED, Azure Kinect)\n"
        "RGB Camera: standard webcams / USB cameras (video feed only)\n"
        "Markered: marker-based tracking — planned, not yet implemented");
    connect(cameraCategoryCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LiveTab::onCameraCategoryChanged);
    addCompactRow(cl, "Category", cameraCategoryCombo_);
    cameraTypeCombo_ = comboWithItems({"ZED2i", "Azure Kinect"});
    connect(cameraTypeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LiveTab::onCameraSettingsChanged);
    addCompactRow(cl, "Camera", cameraTypeCombo_);

    rgbModelCombo_ = new QComboBox;
    rgbModelCombo_->setToolTip(
        "Drop-in tracking model to run on the RGB feed.\n"
        "Models are .onnx + .json pairs in the RGB_Models folder.");
    connect(rgbModelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateViewLayout(); onCameraSettingsChanged(); });
    rgbModelRow_ = addCompactRow(cl, "Model", rgbModelCombo_);
    rgbModelRow_->setVisible(false);

    addSectionHeader(cl, "Camera Settings");
    zedSettingsGroup_ = new QWidget;
    auto* zedLayout = new QVBoxLayout(zedSettingsGroup_);
    zedLayout->setContentsMargins(0, 0, 0, 0);
    zedLayout->setSpacing(3);
    zedBodyFormatCombo_ = comboWithItems({"BODY_18 (18 joints)", "BODY_34 (34 joints)", "BODY_38 (38 joints)"});
    zedTrackingCombo_ = comboWithItems({"Fast", "Medium", "Accurate"});
    zedResolutionCombo_ = comboWithItems({"Auto", "HD720", "HD1080", "HD1200", "HD1536", "HD2K"});
    zedDepthCombo_ = comboWithItems({"Neural Light", "Neural", "Neural Plus"});
    zedBodyFormatCombo_->setCurrentIndex(1);
    zedTrackingCombo_->setCurrentIndex(2);
    zedResolutionCombo_->setCurrentIndex(1);
    zedDepthCombo_->setCurrentIndex(1);
    connect(zedBodyFormatCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCameraSettingsChanged);
    connect(zedTrackingCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCameraSettingsChanged);
    connect(zedResolutionCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCameraSettingsChanged);
    connect(zedDepthCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCameraSettingsChanged);
    addCompactRow(zedLayout, "Body Format", zedBodyFormatCombo_);
    addCompactRow(zedLayout, "Tracking Model", zedTrackingCombo_);
    addCompactRow(zedLayout, "Resolution", zedResolutionCombo_);
    addCompactRow(zedLayout, "Depth Mode", zedDepthCombo_);
    cl->addWidget(zedSettingsGroup_);

    kinectSettingsGroup_ = new QWidget;
    auto* kinectLayout = new QVBoxLayout(kinectSettingsGroup_);
    kinectLayout->setContentsMargins(0, 0, 0, 0);
    kinectLayout->setSpacing(3);
    k4aProcessingCombo_ = comboWithItems({"GPU (Auto)", "CPU", "GPU CUDA", "GPU TensorRT", "GPU DirectML"});
    k4aDepthCombo_ = comboWithItems({"NFOV 2x2 Binned", "NFOV Unbinned", "WFOV 2x2 Binned", "WFOV Unbinned"});
    k4aColorCombo_ = comboWithItems({"720p", "1080p", "1440p", "1536p", "2160p"});
    k4aFpsCombo_ = comboWithItems({"5 FPS", "15 FPS", "30 FPS"});
    k4aOrientationCombo_ = comboWithItems({"Default", "Clockwise 90", "Counter-CW 90", "Flipped 180"});
    k4aDepthCombo_->setCurrentIndex(1);
    k4aFpsCombo_->setCurrentIndex(2);
    connect(k4aProcessingCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCameraSettingsChanged);
    connect(k4aDepthCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCameraSettingsChanged);
    connect(k4aColorCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCameraSettingsChanged);
    connect(k4aFpsCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCameraSettingsChanged);
    connect(k4aOrientationCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCameraSettingsChanged);
    addCompactRow(kinectLayout, "Processing", k4aProcessingCombo_);
    addCompactRow(kinectLayout, "Depth Mode", k4aDepthCombo_);
    addCompactRow(kinectLayout, "Color Res", k4aColorCombo_);
    addCompactRow(kinectLayout, "FPS", k4aFpsCombo_);
    addCompactRow(kinectLayout, "Orientation", k4aOrientationCombo_);
    cl->addWidget(kinectSettingsGroup_);

    addSectionHeader(cl, "Session");
    startCamBtn_ = compactButton("Start Camera");
    pauseBtn_ = compactButton("Pause");
    stopCamBtn_ = compactButton("Stop Camera");
    setPrimary(startCamBtn_);
    setDestructive(stopCamBtn_);
    connect(startCamBtn_, &QPushButton::clicked, this, &LiveTab::onStartCamera);
    connect(pauseBtn_, &QPushButton::clicked, this, &LiveTab::onPauseResume);
    connect(stopCamBtn_, &QPushButton::clicked, this, &LiveTab::onStopCamera);
    cl->addWidget(startCamBtn_);
    cl->addWidget(pauseBtn_);
    cl->addWidget(stopCamBtn_);

    // === Session Info ===
    addSectionHeader(cl, "Session Info");
    patientIdEdit_ = new QLineEdit;
    patientIdEdit_->setPlaceholderText("Patient ID");
    operatorEdit_ = new QLineEdit;
    operatorEdit_->setPlaceholderText("Operator");
    notesEdit_ = new QLineEdit;
    notesEdit_->setPlaceholderText("Notes");
    connect(patientIdEdit_, &QLineEdit::textChanged, this, [this](const QString& s) {
        ctrl_->patientId = s.toStdString();
    });
    connect(operatorEdit_, &QLineEdit::textChanged, this, [this](const QString& s) {
        ctrl_->operatorName = s.toStdString();
    });
    connect(notesEdit_, &QLineEdit::textChanged, this, [this](const QString& s) {
        ctrl_->sessionNotes = s.toStdString();
    });
    addCompactRow(cl, "Patient ID", patientIdEdit_);
    addCompactRow(cl, "Operator", operatorEdit_);
    addCompactRow(cl, "Notes", notesEdit_);

    // === Recording ===
    addSectionHeader(cl, "Recording");
    fileNameEdit_ = new QLineEdit("session");
    connect(fileNameEdit_, &QLineEdit::textChanged, this, [this](const QString& s) {
        ctrl_->recordingFileName = s.toStdString();
    });
    addCompactRow(cl, "File Name", fileNameEdit_);

    recordCb_ = new QCheckBox("Record Joint Positions");
    connect(recordCb_, &QCheckBox::toggled, this, [this](bool v) { ctrl_->recordJoints = v; });
    cl->addWidget(recordCb_);

    recordVideoCb_ = new QCheckBox("Record Camera Footage (MP4)");
    recordVideoCb_->setToolTip(
        "Also record the camera image stream to an MP4 file alongside the\n"
        "joint CSV, so the footage can be reused to test other tracking models.");
    connect(recordVideoCb_, &QCheckBox::toggled, this, [this](bool v) {
        ctrl_->recordVideo = v;
        updateButtons();
    });
    if (!ctrl_->videoRecordingSupported()) {
        recordVideoCb_->setEnabled(false);
        recordVideoCb_->setToolTip("Camera footage recording requires Qt 6.6 or newer.");
    }
    cl->addWidget(recordVideoCb_);

    startRecBtn_ = compactButton("Start Recording");
    stopRecBtn_ = compactButton("Stop Recording");
    setPrimary(startRecBtn_);
    stopRecBtn_->setProperty("recording", true);
    connect(startRecBtn_, &QPushButton::clicked, this, &LiveTab::onStartRecording);
    connect(stopRecBtn_, &QPushButton::clicked, this, &LiveTab::onStopRecording);
    addButtonRow(cl, startRecBtn_, stopRecBtn_);

    countdownCb_ = new QCheckBox("Countdown Before Recording");
    connect(countdownCb_, &QCheckBox::toggled, this, [this](bool v) { ctrl_->countdownEnabled = v; });
    auto* cdWidget = new QWidget;
    auto* cdRow = new QHBoxLayout(cdWidget);
    cdRow->setContentsMargins(0, 0, 0, 0);
    cdRow->setSpacing(5);
    cdRow->addWidget(countdownCb_, 1);
    countdownSecs_ = new QSpinBox;
    countdownSecs_->setRange(1, 30);
    countdownSecs_->setValue(3);
    countdownSecs_->setFixedWidth(52);
    connect(countdownSecs_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        ctrl_->countdownSeconds = v;
    });
    cdRow->addWidget(countdownSecs_);
    cdRow->addWidget(new QLabel("sec"));
    goDisplaySecs_ = new QDoubleSpinBox;
    goDisplaySecs_->setRange(0.5, 10.0);
    goDisplaySecs_->setSingleStep(0.5);
    goDisplaySecs_->setValue(2.0);
    goDisplaySecs_->setFixedWidth(58);
    connect(goDisplaySecs_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        ctrl_->goDisplaySeconds = (float)v;
    });
    cdRow->addWidget(goDisplaySecs_);
    cdRow->addWidget(new QLabel("GO"));
    cl->addWidget(cdWidget);

    // === Display ===
    addSectionHeader(cl, "Display");
    patientScreenCb_ = new QCheckBox("Show Patient Screen");
    patientScreenCb_->setChecked(true);
    patientScreenCb_->setToolTip(
        "Untick to use a single main view — useful when no dedicated\n"
        "patient-facing screen is in use.");
    connect(patientScreenCb_, &QCheckBox::toggled, this, [this](bool) { updateViewLayout(); });
    cl->addWidget(patientScreenCb_);

    showFeedCb_ = new QCheckBox("Show Camera Feed");
    connect(showFeedCb_, &QCheckBox::toggled, this, &LiveTab::onShowFeedToggled);
    cl->addWidget(showFeedCb_);

    smoothingSlider_ = new QDoubleSpinBox;
    smoothingSlider_->setRange(0.0, 0.9);
    smoothingSlider_->setSingleStep(0.05);
    smoothingSlider_->setValue(0.0);
    connect(smoothingSlider_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) { ctrl_->smoothingFactor = (float)v; });
    addCompactRow(cl, "Smoothing", smoothingSlider_);

    showAnglesCb_ = new QCheckBox("Show Joint Angles");
    connect(showAnglesCb_, &QCheckBox::toggled, this, [this](bool v) {
        ctrl_->showJointAngles = v;
    });
    cl->addWidget(showAnglesCb_);

    // === Camera 2 ===
    addSectionHeader(cl, "Camera 2 (Optional)");
    camera2Cb_ = new QCheckBox("Enable Camera 2");
    connect(camera2Cb_, &QCheckBox::toggled, this, [this](bool) {
        updateCameraSettingVisibility();
        updateViewLayout();
        updateButtons();
    });
    cl->addWidget(camera2Cb_);

    camera2Details_ = new QWidget;
    auto* cam2Layout = new QVBoxLayout(camera2Details_);
    cam2Layout->setContentsMargins(0, 0, 0, 0);
    cam2Layout->setSpacing(3);
    camera2CategoryCombo_ = comboWithItems({"Markerless (Depth)", "RGB Camera", "Markered (TODO)"});
    connect(camera2CategoryCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int category) {
                populateDeviceCombo(camera2TypeCombo_, category, /*markerlessDefault=*/1);
                if (category == 1) populateModelCombo(rgbModel2Combo_);
                updateViewLayout();
                onCamera2SettingsChanged();
            });
    addCompactRow(cam2Layout, "Category", camera2CategoryCombo_);
    camera2TypeCombo_ = comboWithItems({"ZED2i", "Azure Kinect"});
    camera2TypeCombo_->setCurrentIndex(1);
    connect(camera2TypeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LiveTab::onCamera2SettingsChanged);
    addCompactRow(cam2Layout, "Camera 2", camera2TypeCombo_);

    rgbModel2Combo_ = new QComboBox;
    connect(rgbModel2Combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateViewLayout(); onCamera2SettingsChanged(); });
    rgbModel2Row_ = addCompactRow(cam2Layout, "Model", rgbModel2Combo_);
    rgbModel2Row_->setVisible(false);

    zed2SettingsGroup_ = new QWidget;
    auto* zed2Layout = new QVBoxLayout(zed2SettingsGroup_);
    zed2Layout->setContentsMargins(0, 0, 0, 0);
    zed2Layout->setSpacing(3);
    zed2BodyFormatCombo_ = comboWithItems({"BODY_18 (18 joints)", "BODY_34 (34 joints)", "BODY_38 (38 joints)"});
    zed2TrackingCombo_ = comboWithItems({"Fast", "Medium", "Accurate"});
    zed2ResolutionCombo_ = comboWithItems({"Auto", "HD720", "HD1080", "HD1200", "HD1536", "HD2K"});
    zed2DepthCombo_ = comboWithItems({"Neural Light", "Neural", "Neural Plus"});
    zed2BodyFormatCombo_->setCurrentIndex(1);
    zed2TrackingCombo_->setCurrentIndex(2);
    zed2ResolutionCombo_->setCurrentIndex(1);
    zed2DepthCombo_->setCurrentIndex(1);
    connect(zed2BodyFormatCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCamera2SettingsChanged);
    connect(zed2TrackingCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCamera2SettingsChanged);
    connect(zed2ResolutionCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCamera2SettingsChanged);
    connect(zed2DepthCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCamera2SettingsChanged);
    addCompactRow(zed2Layout, "Body Format", zed2BodyFormatCombo_);
    addCompactRow(zed2Layout, "Tracking Model", zed2TrackingCombo_);
    addCompactRow(zed2Layout, "Resolution", zed2ResolutionCombo_);
    addCompactRow(zed2Layout, "Depth Mode", zed2DepthCombo_);
    cam2Layout->addWidget(zed2SettingsGroup_);

    kinect2SettingsGroup_ = new QWidget;
    auto* kinect2Layout = new QVBoxLayout(kinect2SettingsGroup_);
    kinect2Layout->setContentsMargins(0, 0, 0, 0);
    kinect2Layout->setSpacing(3);
    k4a2ProcessingCombo_ = comboWithItems({"GPU (Auto)", "CPU", "GPU CUDA", "GPU TensorRT", "GPU DirectML"});
    k4a2DepthCombo_ = comboWithItems({"NFOV 2x2 Binned", "NFOV Unbinned", "WFOV 2x2 Binned", "WFOV Unbinned"});
    k4a2ColorCombo_ = comboWithItems({"720p", "1080p", "1440p", "1536p", "2160p"});
    k4a2FpsCombo_ = comboWithItems({"5 FPS", "15 FPS", "30 FPS"});
    k4a2OrientationCombo_ = comboWithItems({"Default", "Clockwise 90", "Counter-CW 90", "Flipped 180"});
    k4a2DepthCombo_->setCurrentIndex(1);
    k4a2FpsCombo_->setCurrentIndex(2);
    connect(k4a2ProcessingCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCamera2SettingsChanged);
    connect(k4a2DepthCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCamera2SettingsChanged);
    connect(k4a2ColorCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCamera2SettingsChanged);
    connect(k4a2FpsCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCamera2SettingsChanged);
    connect(k4a2OrientationCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LiveTab::onCamera2SettingsChanged);
    addCompactRow(kinect2Layout, "Processing", k4a2ProcessingCombo_);
    addCompactRow(kinect2Layout, "Depth Mode", k4a2DepthCombo_);
    addCompactRow(kinect2Layout, "Color Res", k4a2ColorCombo_);
    addCompactRow(kinect2Layout, "FPS", k4a2FpsCombo_);
    addCompactRow(kinect2Layout, "Orientation", k4a2OrientationCombo_);
    cam2Layout->addWidget(kinect2SettingsGroup_);

    startCam2Btn_ = compactButton("Start Camera 2");
    stopCam2Btn_ = compactButton("Stop Cam 2");
    setPrimary(startCam2Btn_);
    setDestructive(stopCam2Btn_);
    connect(startCam2Btn_, &QPushButton::clicked, this, &LiveTab::onStartCamera2);
    connect(stopCam2Btn_, &QPushButton::clicked, this, &LiveTab::onStopCamera2);
    addButtonRow(cam2Layout, startCam2Btn_, stopCam2Btn_);
    cl->addWidget(camera2Details_);

    // === Biofeedback ===
    biofeedbackSection_ = new QWidget;
    auto* bioLayout = new QVBoxLayout(biofeedbackSection_);
    bioLayout->setContentsMargins(0, 0, 0, 0);
    bioLayout->setSpacing(3);
    addSectionHeader(bioLayout, "EXPERIMENTAL: Biofeedback");
    auto* bioLabel = new QLabel("Biofeedback ACTIVE");
    bioLabel->setProperty("warning", true);
    bioLayout->addWidget(bioLabel);
    showModifiedCb_ = new QCheckBox("Show Modified in Viewer");
    connect(showModifiedCb_, &QCheckBox::toggled, this, [this](bool v) {
        ctrl_->showModifiedInViewer = v;
    });
    bioLayout->addWidget(showModifiedCb_);
    biofeedbackSection_->setVisible(false);
    cl->addWidget(biofeedbackSection_);

    // === Overlay ===
    addSectionHeader(cl, "Patient Screen Overlay");
    overlayTextEdit_ = new QLineEdit;
    overlayTextEdit_->setPlaceholderText("Display text");
    connect(overlayTextEdit_, &QLineEdit::textChanged, this, &LiveTab::onOverlayChanged);
    addCompactRow(cl, "Display Text", overlayTextEdit_);

    auto* overlayRowWidget = new QWidget;
    auto* overlayRow1 = new QHBoxLayout(overlayRowWidget);
    overlayRow1->setContentsMargins(0, 0, 0, 0);
    overlayRow1->setSpacing(5);
    overlayPositionCombo_ = comboWithItems({"Top", "Middle", "Bottom"});
    overlayScaleSpin_ = new QDoubleSpinBox;
    overlayScaleSpin_->setRange(0.5, 5.0);
    overlayScaleSpin_->setSingleStep(0.1);
    overlayScaleSpin_->setValue(1.0);
    overlayColorCombo_ = comboWithItems({"White", "Red", "Green", "Blue", "Yellow", "Cyan"});
    for (auto* combo : {overlayPositionCombo_, overlayColorCombo_}) {
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &LiveTab::onOverlayChanged);
    }
    connect(overlayScaleSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &LiveTab::onOverlayChanged);
    overlayRow1->addWidget(overlayPositionCombo_);
    overlayRow1->addWidget(new QLabel("Position"));
    overlayScaleSpin_->setFixedWidth(58);
    overlayRow1->addWidget(overlayScaleSpin_);
    overlayRow1->addWidget(new QLabel("Size"));
    overlayRow1->addWidget(overlayColorCombo_);
    overlayRow1->addWidget(new QLabel("Color"));
    cl->addWidget(overlayRowWidget);

    auto* shapeWidget = new QWidget;
    auto* shapeLayout = new QHBoxLayout(shapeWidget);
    shapeLayout->setContentsMargins(0, 0, 0, 0);
    shapeLayout->setSpacing(5);
    shapeLayout->addWidget(new QLabel("Shape:"));
    shapeNoneRb_ = new QRadioButton("None");
    shapeCircleRb_ = new QRadioButton("Circle");
    shapeSquareRb_ = new QRadioButton("Square");
    shapeTriangleRb_ = new QRadioButton("Triangle");
    shapeCircleRb_->setProperty("shapeColor", "red");
    shapeSquareRb_->setProperty("shapeColor", "blue");
    shapeTriangleRb_->setProperty("shapeColor", "green");
    shapeNoneRb_->setChecked(true);
    for (auto* rb : {shapeNoneRb_, shapeCircleRb_, shapeSquareRb_, shapeTriangleRb_}) {
        connect(rb, &QRadioButton::toggled, this, [this](bool checked) {
            if (checked) onOverlayChanged();
        });
        shapeLayout->addWidget(rb);
    }
    shapeLayout->addStretch();
    cl->addWidget(shapeWidget);

    // === Pop-out ===
    addSectionHeader(cl, "Patient Screen");
    popOutBtn_ = compactButton("Pop Out Patient Screen");
    connect(popOutBtn_, &QPushButton::clicked, this, &LiveTab::onPopOut);
    cl->addWidget(popOutBtn_);

    // Debug bar (session time, FPS)
    debugLabel_ = new QLabel;
    debugLabel_->setProperty("muted", true);
    cl->addWidget(debugLabel_);

    cl->addStretch();
    splitter->addWidget(controlsScroll);

    splitter->setStretchFactor(0, 38);
    splitter->setStretchFactor(1, 35);
    splitter->setStretchFactor(2, 35); // camera feed (hidden unless RGB category)
    splitter->setStretchFactor(3, 27);
    splitter->setSizes({720, 660, 660, 520});

    // Signals
    connect(ctrl_, &AppController::sessionStateChanged, this, &LiveTab::onSessionStateChanged);
    connect(ctrl_, &AppController::protocolActionFired, this, &LiveTab::onProtocolActionFired);
    connect(ctrl_, &AppController::protocolsChanged, this, [this] { refreshProtocolList(); });
    connect(ctrl_, &AppController::overlayChanged, this, [this]() {
        patientScreen_->setOverlayState(ctrl_->overlayState());
        if (popout_) popout_->view()->setOverlayState(ctrl_->overlayState());
    });

    // Tick for updating skeleton views + debug bar
    tickTimer_ = new QTimer(this);
    tickTimer_->setInterval(33);
    connect(tickTimer_, &QTimer::timeout, this, &LiveTab::onTick);
    tickTimer_->start();

    applyCameraSettings(0);
    applyCameraSettings(1);
    updateCameraSettingVisibility();
    refreshProtocolList();
    updateButtons();
    updateStatus();
}

LiveTab::~LiveTab() {
    if (popout_) { popout_->close(); delete popout_; popout_ = nullptr; }
    if (feedWindow_) { feedWindow_->close(); delete feedWindow_; feedWindow_ = nullptr; }
}

void LiveTab::onTick() {
    // Grab the latest frame for slot 0 and 1
    std::shared_ptr<FrameData> f0, f1;
    {
        auto& slot0 = ctrl_->slot(0);
        std::lock_guard<std::mutex> lock(slot0.frameMutex);
        f0 = slot0.sharedFrame;
    }
    {
        auto& slot1 = ctrl_->slot(1);
        std::lock_guard<std::mutex> lock(slot1.frameMutex);
        f1 = slot1.sharedFrame;
    }

    // Inline camera feed (main view for RGB cameras) shows the raw image
    // of whichever slot is the RGB camera
    if (feedPanel_->isVisible()) feedView_->setFrame(feedSlot_ == 0 ? f0 : f1);

    // Apply smoothing (we mutate a copy so the original in slot remains raw)
    if (f0) {
        auto fCopy = std::make_shared<FrameData>(*f0);
        ctrl_->applySmoothingToFrame(*fCopy, 0);

        // Apply biofeedback transforms for patient screen
        std::shared_ptr<FrameData> patientFrame = fCopy;
        if (ctrl_->biofeedbackActive() && ctrl_->biofeedbackEngine().hasActiveTransforms()) {
            float protoTime = (ctrl_->protocolRunner().state() == ProtocolRunnerState::Running)
                ? ctrl_->protocolRunner().currentTime()
                : ctrl_->debugInfo.sessionSeconds;
            auto modified = std::make_shared<FrameData>(
                ctrl_->biofeedbackEngine().applyTransforms(*fCopy, protoTime));
            patientFrame = modified;
        }

        // Patient screen respects avatar visibility
        patientScreen_->setAvatarVisible(ctrl_->avatarVisible());
        patientScreen_->setOverlayState(ctrl_->overlayState());
        patientScreen_->setShowJointAngles(ctrl_->showJointAngles);
        patientScreen_->setFrame(patientFrame);

        // Patient viewer: dual overlay if both cameras active, else single
        if (ctrl_->slot(1).enabled && f1) {
            auto f1Copy = std::make_shared<FrameData>(*f1);
            ctrl_->applySmoothingToFrame(*f1Copy, 1);
            patientViewer_->setOverlayFrames(fCopy, f1Copy);
        } else {
            patientViewer_->clearOverlay();
            patientViewer_->setFrame(ctrl_->showModifiedInViewer ? patientFrame : fCopy);
        }

        // Pop-out (if open)
        if (popout_ && popout_->isVisible()) {
            popout_->view()->setAvatarVisible(ctrl_->avatarVisible());
            popout_->view()->setOverlayState(ctrl_->overlayState());
            popout_->view()->setShowJointAngles(ctrl_->showJointAngles);
            popout_->view()->setFrame(patientFrame);
        }
    }

    // Update debug label
    const auto& d = ctrl_->debugInfo;
    int ss = (int)d.sessionSeconds;
    QString dbg = QString("Cam1: %1 FPS | Bodies: %2  |  Session: %3:%4")
        .arg(d.cameraFps[0], 0, 'f', 0)
        .arg(d.bodyCount[0])
        .arg(ss / 60).arg(ss % 60, 2, 10, QChar('0'));
    if (ctrl_->slot(1).enabled) {
        dbg += QString("  |  Cam2: %1 FPS (%2 bodies)")
            .arg(d.cameraFps[1], 0, 'f', 0).arg(d.bodyCount[1]);
    }
    if (d.isRecording) {
        dbg += QString("  |  <span style='color:#e05050;'>REC</span>");
    }
    if (ctrl_->protocolRunner().state() == ProtocolRunnerState::Running) {
        int ps = (int)ctrl_->protocolRunner().currentTime();
        dbg += QString("  |  <span style='color:#50b0e0;'>Protocol %1:%2</span>")
            .arg(ps / 60).arg(ps % 60, 2, 10, QChar('0'));
    }
    debugLabel_->setText(dbg);

    if (biofeedbackSection_) {
        biofeedbackSection_->setVisible(ctrl_->biofeedbackActive());
    }

    updateProtocolUI();
    updateButtons();
}

void LiveTab::updateButtons() {
    bool stopped = (ctrl_->sessionState() == SessionState::Stopped);
    bool running = (ctrl_->sessionState() == SessionState::Running);
    bool isRecording = ctrl_->isAnyRecording();
    bool protoRunning = (ctrl_->protocolRunner().state() == ProtocolRunnerState::Running);
    auto slotAvailable = [this](QComboBox* categoryCombo, QComboBox* typeCombo) {
        switch (categoryCombo->currentIndex()) {
            case 1: return rgbCamerasPresent_;
            case 2: return false; // Markered: TODO
            default:
                return typeCombo->currentIndex() == 0 ? ctrl_->zedSdkAvailable()
                                                      : ctrl_->kinectSdkAvailable();
        }
    };
    const bool cam1Available = slotAvailable(cameraCategoryCombo_, cameraTypeCombo_);
    const bool cam2Available = slotAvailable(camera2CategoryCombo_, camera2TypeCombo_);

    startCamBtn_->setEnabled(stopped && cam1Available && !protoRunning);
    stopCamBtn_->setEnabled(!stopped && !protoRunning);
    pauseBtn_->setEnabled(!stopped);
    pauseBtn_->setText(ctrl_->sessionState() == SessionState::Paused ? "Resume" : "Pause");

    fileNameEdit_->setEnabled(!isRecording);
    startRecBtn_->setEnabled(running && !isRecording &&
                             (ctrl_->recordJoints || ctrl_->recordVideo));
    stopRecBtn_->setEnabled(isRecording);

    const bool cam2Wanted = camera2Cb_->isChecked();
    camera2TypeCombo_->setEnabled(cam2Wanted && !ctrl_->slot(1).enabled && !protoRunning);
    startCam2Btn_->setEnabled(cam2Wanted && cam2Available && !ctrl_->slot(1).enabled && !protoRunning);
    stopCam2Btn_->setEnabled(ctrl_->slot(1).enabled && !protoRunning);

    patientIdEdit_->setEnabled(stopped);
    operatorEdit_->setEnabled(stopped);
    notesEdit_->setEnabled(stopped);

    bool protoLoaded = ctrl_->isProtocolLoaded();
    loadProtocolBtn_->setEnabled(!protoRunning && !protoLoaded);
    unloadProtocolBtn_->setEnabled(!protoRunning && protoLoaded);
    runProtocolBtn_->setEnabled(!protoRunning && protoLoaded);
    abortProtocolBtn_->setEnabled(protoRunning);
    protocolCombo_->setEnabled(!protoRunning && !protoLoaded);
}

void LiveTab::updateStatus() {
    switch (ctrl_->sessionState()) {
        case SessionState::Running:
            statusLabel_->setText("<span style='color:#33e633;'>LIVE</span>");
            return;
        case SessionState::Paused:
            statusLabel_->setText("<span style='color:#ffcc1a;'>PAUSED</span>");
            return;
        default:
            statusLabel_->setText("<span style='color:#9a9a9a;'>STOPPED</span>");
            return;
    }

}

void LiveTab::updateProtocolUI() {
    auto& runner = ctrl_->protocolRunner();
    bool running = (runner.state() == ProtocolRunnerState::Running);

    if (running) {
        protocolProgress_->setVisible(true);
        float progress = runner.totalDuration() > 0
            ? runner.currentTime() / runner.totalDuration() : 0;
        protocolProgress_->setValue((int)(progress * 100));

        const auto& events = runner.protocol().events;
        int curIdx = runner.currentEventIndex();
        int nextIdx = curIdx + 1;

        protocolInfoLabel_->setText(QString("Event %1 / %2")
            .arg(curIdx + 1).arg(runner.totalEvents()));

        // Current event
        if (curIdx >= 0 && curIdx < (int)events.size()) {
            const auto& ev = events[curIdx];
            int mm = (int)ev.timeOffsetSeconds / 60;
            int ss = (int)ev.timeOffsetSeconds % 60;
            QString paramShort = QString::fromStdString(ev.parameter);
            int pipe = paramShort.indexOf('|');
            if (pipe >= 0) paramShort = paramShort.left(pipe);
            if (paramShort.length() > 50) paramShort = paramShort.left(50) + "…";

            currentEventLabel_->setText(QString(
                "<b>Current [%1:%2]</b><br/>%3<br/><span style='color:#999;'>%4</span>")
                .arg(mm).arg(ss, 2, 10, QChar('0'))
                .arg(protocolEventTypeName(ev.type))
                .arg(paramShort));
            currentBox_->setVisible(true);
        } else {
            currentBox_->setVisible(false);
        }

        // Next event + countdown
        if (nextIdx >= 0 && nextIdx < (int)events.size()) {
            const auto& nev = events[nextIdx];
            float countdown = nev.timeOffsetSeconds - runner.currentTime();
            if (countdown < 0) countdown = 0;
            if (countdown >= 60) {
                int cm = (int)countdown / 60;
                int cs = (int)countdown % 60;
                countdownLabel_->setText(QString("Next Event in %1:%2").arg(cm).arg(cs, 2, 10, QChar('0')));
            } else {
                countdownLabel_->setText(QString("Next Event in %1 s").arg(countdown, 0, 'f', 0));
            }
            countdownLabel_->setVisible(true);

            int nm = (int)nev.timeOffsetSeconds / 60;
            int ns = (int)nev.timeOffsetSeconds % 60;
            QString np = QString::fromStdString(nev.parameter);
            int pipe = np.indexOf('|');
            if (pipe >= 0) np = np.left(pipe);
            if (np.length() > 50) np = np.left(50) + "…";

            nextEventLabel_->setText(QString(
                "<b>Upcoming [%1:%2]</b><br/>%3<br/><span style='color:#999;'>%4</span>")
                .arg(nm).arg(ns, 2, 10, QChar('0'))
                .arg(protocolEventTypeName(nev.type))
                .arg(np));
            nextBox_->setVisible(true);
        } else {
            countdownLabel_->setText("Final event reached.");
            countdownLabel_->setVisible(true);
            nextBox_->setVisible(false);
        }
    } else {
        protocolProgress_->setVisible(false);
        currentBox_->setVisible(false);
        nextBox_->setVisible(false);
        countdownLabel_->setVisible(false);

        if (ctrl_->isProtocolLoaded()) {
            protocolInfoLabel_->setText(QString("Loaded: %1\nEvents: %2")
                .arg(QString::fromStdString(ctrl_->loadedProtocol().name))
                .arg((int)ctrl_->loadedProtocol().events.size()));
        } else {
            protocolInfoLabel_->setText("No protocol loaded.");
        }
    }
}

void LiveTab::refreshProtocolList() {
    const QString current = protocolCombo_->currentText();
    protocolCombo_->clear();
    auto files = listProtocolFiles(ctrl_->config().protocolsDir);
    for (auto& f : files) protocolCombo_->addItem(QString::fromStdString(f));
    const int idx = protocolCombo_->findText(current);
    if (idx >= 0) protocolCombo_->setCurrentIndex(idx);
}

void LiveTab::updateCameraSettingVisibility() {
    const bool cam1Markerless = cameraCategoryCombo_->currentIndex() == 0;
    const bool cam1Zed = cam1Markerless && cameraTypeCombo_->currentIndex() == 0;
    const bool cam2Markerless = camera2CategoryCombo_->currentIndex() == 0;
    const bool cam2Zed = cam2Markerless && camera2TypeCombo_->currentIndex() == 0;
    zedSettingsGroup_->setVisible(cam1Markerless && cam1Zed);
    kinectSettingsGroup_->setVisible(cam1Markerless && !cam1Zed);
    rgbModelRow_->setVisible(cameraCategoryCombo_->currentIndex() == 1);
    const bool cam2Visible = camera2Cb_->isChecked() || ctrl_->slot(1).enabled;
    camera2Details_->setVisible(cam2Visible);
    zed2SettingsGroup_->setVisible(cam2Visible && cam2Markerless && cam2Zed);
    kinect2SettingsGroup_->setVisible(cam2Visible && cam2Markerless && !cam2Zed);
    rgbModel2Row_->setVisible(cam2Visible && camera2CategoryCombo_->currentIndex() == 1);

    const bool primaryStopped = ctrl_->sessionState() == SessionState::Stopped;
    cameraCategoryCombo_->setEnabled(primaryStopped);
    cameraTypeCombo_->setEnabled(primaryStopped);
    zedSettingsGroup_->setEnabled(primaryStopped);
    kinectSettingsGroup_->setEnabled(primaryStopped);
    zed2SettingsGroup_->setEnabled(!ctrl_->slot(1).enabled);
    kinect2SettingsGroup_->setEnabled(!ctrl_->slot(1).enabled);
}

void LiveTab::applyCameraSettings(int slotIndex) {
    const bool slot2 = slotIndex == 1;
    QComboBox* typeCombo = slot2 ? camera2TypeCombo_ : cameraTypeCombo_;
    CameraConfig cfg;
    cfg.deviceIndex = slotIndex;

    const int category = slot2 ? camera2CategoryCombo_->currentIndex()
                               : cameraCategoryCombo_->currentIndex();
    if (category == 1) { // RGB webcam, optionally with a tracking model
        ctrl_->setCameraType(slotIndex, CameraType::RGBWebcam);
        cfg.rgbDeviceIndex = std::max(0, typeCombo->currentIndex());
        QComboBox* modelCombo = slot2 ? rgbModel2Combo_ : rgbModelCombo_;
        cfg.rgbModelOnnx = modelCombo->currentData().toString().toStdString();
        ctrl_->setCameraConfig(slotIndex, cfg);
        return;
    }
    if (category == 2) return; // Markered: not implemented yet

    if (typeCombo->currentIndex() == 0) {
        ctrl_->setCameraType(slotIndex, CameraType::ZED2i);
        cfg.zedBodyFormat = (ZedBodyFormat)(slot2 ? zed2BodyFormatCombo_->currentIndex()
                                                  : zedBodyFormatCombo_->currentIndex());
        cfg.zedTrackingModel = (ZedTrackingModel)(slot2 ? zed2TrackingCombo_->currentIndex()
                                                        : zedTrackingCombo_->currentIndex());
        cfg.zedResolution = (ZedResolution)(slot2 ? zed2ResolutionCombo_->currentIndex()
                                                  : zedResolutionCombo_->currentIndex());
        cfg.zedDepthMode = (ZedDepthMode)(slot2 ? zed2DepthCombo_->currentIndex()
                                                : zedDepthCombo_->currentIndex());
    } else {
        ctrl_->setCameraType(slotIndex, CameraType::AzureKinect);
        cfg.k4aProcessingMode = (K4AProcessingMode)(slot2 ? k4a2ProcessingCombo_->currentIndex()
                                                          : k4aProcessingCombo_->currentIndex());
        cfg.k4aDepthMode = (K4ADepthMode)(slot2 ? k4a2DepthCombo_->currentIndex()
                                                : k4aDepthCombo_->currentIndex());
        cfg.k4aColorRes = (K4AColorRes)(slot2 ? k4a2ColorCombo_->currentIndex()
                                              : k4aColorCombo_->currentIndex());
        cfg.k4aFps = (K4AFPS)(slot2 ? k4a2FpsCombo_->currentIndex()
                                    : k4aFpsCombo_->currentIndex());
        cfg.k4aSensorOrientation = (K4ASensorOrientation)(slot2 ? k4a2OrientationCombo_->currentIndex()
                                                                : k4aOrientationCombo_->currentIndex());
    }

    ctrl_->setCameraConfig(slotIndex, cfg);
}

void LiveTab::onCameraCategoryChanged() {
    populateCameraDeviceCombo();
    if (cameraCategoryCombo_->currentIndex() == 1) populateModelCombo(rgbModelCombo_);
    // Non-markerless categories have no patient-facing avatar, so default
    // to the single main view; the user can re-enable the patient screen.
    patientScreenCb_->setChecked(cameraCategoryCombo_->currentIndex() == 0);
    updateViewLayout();
    onCameraSettingsChanged();
}

void LiveTab::updateViewLayout() {
    if (!camera2CategoryCombo_) return; // construction not finished yet

    const bool cam1Rgb = cameraCategoryCombo_->currentIndex() == 1;
    const bool cam1Markerless = cameraCategoryCombo_->currentIndex() == 0;
    const bool cam2Active = camera2Cb_ && (camera2Cb_->isChecked() || ctrl_->slot(1).enabled);
    const bool cam2Rgb = cam2Active && camera2CategoryCombo_->currentIndex() == 1;
    const bool cam2Markerless = cam2Active && camera2CategoryCombo_->currentIndex() == 0;

    // Mixed setups show both: the feed pane for the RGB slot and the
    // skeleton viewer for the markerless slot. An RGB slot with a tracking
    // model selected produces a skeleton too.
    const bool cam1Model = cam1Rgb && rgbModelCombo_ &&
                           !rgbModelCombo_->currentData().toString().isEmpty();
    const bool cam2Model = cam2Rgb && rgbModel2Combo_ &&
                           !rgbModel2Combo_->currentData().toString().isEmpty();
    const bool anyRgb = cam1Rgb || cam2Rgb;
    const bool anyMarkerless = cam1Markerless || cam2Markerless;
    feedSlot_ = cam1Rgb ? 0 : 1;
    feedPanel_->setVisible(anyRgb);
    patientViewerPanel_->setVisible(anyMarkerless || cam1Model || cam2Model || !anyRgb);
    patientScreenPanel_->setVisible(patientScreenCb_->isChecked());
}

void LiveTab::populateCameraDeviceCombo() {
    populateDeviceCombo(cameraTypeCombo_, cameraCategoryCombo_->currentIndex(),
                        /*markerlessDefault=*/0);
}

void LiveTab::populateModelCombo(QComboBox* modelCombo) {
    modelCombo->blockSignals(true);
    modelCombo->clear();
    modelCombo->addItem("None (video only)", QString());
    for (const auto& m : listRgbModels(ctrl_->config().rgbModelsDir))
        modelCombo->addItem(QString::fromStdString(m.name),
                            QString::fromStdString(m.onnxPath));
    modelCombo->blockSignals(false);
}

void LiveTab::populateDeviceCombo(QComboBox* typeCombo, int category, int markerlessDefault) {
    typeCombo->blockSignals(true);
    typeCombo->clear();
    switch (category) {
        case 0: // Markerless depth cameras
            typeCombo->addItems({"ZED2i", "Azure Kinect"});
            typeCombo->setCurrentIndex(markerlessDefault);
            break;
        case 1: { // RGB cameras — enumerate connected webcams / USB cameras
            const auto devices = QMediaDevices::videoInputs();
            rgbCamerasPresent_ = !devices.isEmpty();
            for (const auto& d : devices)
                typeCombo->addItem(d.description());
            if (devices.isEmpty())
                typeCombo->addItem("No RGB cameras detected");
            break;
        }
        case 2: // Markered — placeholder for future marker-based systems
            typeCombo->addItem("Marker-based tracking — coming soon");
            break;
    }
    typeCombo->blockSignals(false);
}

void LiveTab::onCameraSettingsChanged() {
    applyCameraSettings(0);
    updateCameraSettingVisibility();
    updateButtons();
}

void LiveTab::onCamera2SettingsChanged() {
    applyCameraSettings(1);
    updateCameraSettingVisibility();
    updateButtons();
}

void LiveTab::onOverlayChanged() {
    TextPosition pos = TextPosition::Top;
    if (overlayPositionCombo_->currentIndex() == 1) pos = TextPosition::Middle;
    else if (overlayPositionCombo_->currentIndex() == 2) pos = TextPosition::Bottom;

    QColor color(255, 255, 255);
    switch (overlayColorCombo_->currentIndex()) {
        case 1: color = QColor(255, 80, 80); break;
        case 2: color = QColor(80, 230, 80); break;
        case 3: color = QColor(80, 130, 255); break;
        case 4: color = QColor(255, 230, 50); break;
        case 5: color = QColor(80, 230, 230); break;
        default: break;
    }

    ctrl_->setOverlayTextFull(overlayTextEdit_->text().toStdString(), pos,
                              (float)overlayScaleSpin_->value(), color);
    if (shapeCircleRb_->isChecked()) ctrl_->setOverlayShape(OverlayShape::RedCircle);
    else if (shapeSquareRb_->isChecked()) ctrl_->setOverlayShape(OverlayShape::BlueSquare);
    else if (shapeTriangleRb_->isChecked()) ctrl_->setOverlayShape(OverlayShape::GreenTriangle);
    else ctrl_->setOverlayShape(OverlayShape::None);
}

void LiveTab::onShowFeedToggled(bool enabled) {
    if (enabled) {
        if (!feedWindow_) feedWindow_ = new CameraFeedWindow(ctrl_);
        feedWindow_->show();
        feedWindow_->raise();
    } else if (feedWindow_) {
        feedWindow_->hide();
    }
}

void LiveTab::onStartCamera() {
    applyCameraSettings(0);
    ctrl_->startCamera(0);
}
void LiveTab::onStopCamera() {
    ctrl_->stopRecording();
    ctrl_->stopCamera(0);
}
void LiveTab::onPauseResume() {
    ctrl_->togglePause();
}
void LiveTab::onStartRecording() {
    if (ctrl_->countdownEnabled && ctrl_->countdownSeconds > 0) {
        ctrl_->countdownActive = true;
        ctrl_->countdownRemaining = ctrl_->countdownSeconds;
        ctrl_->goDisplaySeconds = (float)goDisplaySecs_->value();
        ctrl_->goDisplayActive = false;
        ctrl_->countdownStart = std::chrono::steady_clock::now();
    } else {
        ctrl_->startRecording();
    }
}
void LiveTab::onStopRecording() { ctrl_->stopRecording(); }
void LiveTab::onStartCamera2() {
    applyCameraSettings(1);
    ctrl_->startCamera(1);
}
void LiveTab::onStopCamera2() { ctrl_->stopCamera(1); }

void LiveTab::onPopOut() {
    if (!popout_) popout_ = new PopoutWindow;
    if (popout_->isVisible()) {
        popout_->hide();
        popOutBtn_->setText("Pop Out Patient Screen");
    } else {
        popout_->view()->setOverlayState(ctrl_->overlayState());
        popout_->view()->setShowJointAngles(ctrl_->showJointAngles);
        popout_->show();
        popOutBtn_->setText("Pop In Patient Screen");
    }
}

void LiveTab::onLoadProtocol() {
    QString name = protocolCombo_->currentText();
    if (name.isEmpty()) return;
    std::string path = ctrl_->config().protocolsDir + "/" + name.toStdString();
    Protocol p;
    if (p.loadJSON(path)) {
        ctrl_->loadProtocol(p);
        recordCb_->setChecked(ctrl_->recordJoints);
        updateProtocolUI();
    } else {
        QMessageBox::warning(this, "Protocol", "Failed to load protocol file.");
    }
}

void LiveTab::onUnloadProtocol() {
    ctrl_->unloadProtocol();
    updateProtocolUI();
}

void LiveTab::onRunProtocol() {
    if (ctrl_->patientId.empty()) {
        QMessageBox::warning(this, "Protocol",
            "Patient ID is required before running a protocol.");
        return;
    }
    ctrl_->runProtocol();
}

void LiveTab::onAbortProtocol() {
    ctrl_->abortProtocol();
    updateProtocolUI();
}

void LiveTab::onProtocolActionFired(QString name) {
    // Future nicety: flash the event in status.
    Q_UNUSED(name);
    updateProtocolUI();
}

void LiveTab::onSessionStateChanged(int) {
    updateButtons();
    updateStatus();
}
