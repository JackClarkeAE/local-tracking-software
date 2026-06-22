#include "experimental_tab.h"
#include "block_game_view.h"
#include "../app_controller.h"
#include "../biofeedback/biofeedback_engine.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QCheckBox>
#include <QPushButton>
#include <QTimer>
#include <QScrollArea>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <mutex>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

ExperimentalTab::ExperimentalTab(AppController* ctrl, QWidget* parent)
    : QWidget(parent), ctrl_(ctrl) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    root->addWidget(scroll);

    auto* content = new QWidget;
    scroll->setWidget(content);
    auto* layout = new QVBoxLayout(content);

    auto* header = new QLabel("<h2 style='color:#e07030;'>EXPERIMENTAL FEATURES</h2>");
    layout->addWidget(header);
    auto* warning = new QLabel("These features are for development and testing only. "
                               "They may change or be removed in future versions.");
    warning->setWordWrap(true);
    warning->setStyleSheet("color: #aaa;");
    layout->addWidget(warning);

    // === Binary Angle Export ===
    auto* binGroup = new QGroupBox("Binary Angle Export");
    binGroup->setStyleSheet("QGroupBox { color: #e0a030; font-weight: bold; }");
    auto* binLayout = new QVBoxLayout(binGroup);

    binaryExportCb_ = new QCheckBox("Export Knee Flexion to Binary File");
    binaryExportCb_->setToolTip("Writes right knee flexion angle and confidence\n"
                                "to a binary file on the desktop every frame.\n"
                                "Format: 2 floats (angle + confidence) = 8 bytes.\n"
                                "File is overwritten each frame for real-time IPC.");
    connect(binaryExportCb_, &QCheckBox::toggled, this, &ExperimentalTab::onBinaryExportToggled);
    binLayout->addWidget(binaryExportCb_);

    binaryStatusLabel_ = new QLabel;
    binaryPathLabel_ = new QLabel;
    binaryPathLabel_->setStyleSheet("color: #888;");
    binLayout->addWidget(binaryStatusLabel_);
    binLayout->addWidget(binaryPathLabel_);

    auto* formatLabel = new QLabel(
        "<b>Binary format (8 bytes):</b>"
        "<ul>"
        "<li>float kneeFlexion (degrees, 0 = straight)</li>"
        "<li>float confidence (0.0-1.0, avg of 3 joints)</li>"
        "</ul>");
    binLayout->addWidget(formatLabel);

    layout->addWidget(binGroup);

    // === Legacy Playback ===
    auto* legacyGroup = new QGroupBox("Legacy Data Playback");
    legacyGroup->setStyleSheet("QGroupBox { color: #e0a030; font-weight: bold; }");
    auto* legacyLayout = new QVBoxLayout(legacyGroup);

    legacyPlaybackCb_ = new QCheckBox("Enable Legacy Azure JSON Playback");
    legacyPlaybackCb_->setToolTip("Enables 'Load Legacy Data' button in the Playback tab.\n"
                                  "Supports older Azure Kinect JSON format with\n"
                                  "different joint ordering and mm-scale positions.");
    connect(legacyPlaybackCb_, &QCheckBox::toggled, this, &ExperimentalTab::legacyPlaybackToggled);
    legacyLayout->addWidget(legacyPlaybackCb_);
    layout->addWidget(legacyGroup);

    // === Resampled Playback ===
    auto* resampledGroup = new QGroupBox("Resampled Azure Data");
    resampledGroup->setStyleSheet("QGroupBox { color: #e0a030; font-weight: bold; }");
    auto* resampledLayout = new QVBoxLayout(resampledGroup);

    resampledPlaybackCb_ = new QCheckBox("Enable Resampled 30FPS Azure Playback");
    resampledPlaybackCb_->setToolTip("Enables 'Load Resampled Data' button in the Playback tab.\n"
                                     "Supports resampled 30FPS Azure Kinect CSV files\n"
                                     "with angle columns and named joint positions.");
    connect(resampledPlaybackCb_, &QCheckBox::toggled, this, &ExperimentalTab::resampledPlaybackToggled);
    resampledLayout->addWidget(resampledPlaybackCb_);
    layout->addWidget(resampledGroup);

    // === Session Report ===
    auto* reportGroup = new QGroupBox("Session Report");
    reportGroup->setStyleSheet("QGroupBox { color: #e0a030; font-weight: bold; }");
    auto* reportLayout = new QVBoxLayout(reportGroup);
    reportTabCb_ = new QCheckBox("Enable Report Tab");
    reportTabCb_->setToolTip("Adds a 'Report' tab that builds a PDF of biomechanical\n"
                             "metrics and graphs from a recorded session.");
    connect(reportTabCb_, &QCheckBox::toggled, this, &ExperimentalTab::reportTabToggled);
    reportLayout->addWidget(reportTabCb_);
    layout->addWidget(reportGroup);

    // === Block Dodge Game ===
    auto* gameGroup = new QGroupBox("Block Dodge Game");
    gameGroup->setStyleSheet("QGroupBox { color: #e0a030; font-weight: bold; }");
    auto* gameLayout = new QVBoxLayout(gameGroup);
    blockGameCb_ = new QCheckBox("Enable Block Dodge Game");
    blockGameCb_->setToolTip("Move your hands to avoid incoming blocks. Requires an active camera.");
    gameLayout->addWidget(blockGameCb_);

    auto* gameButtons = new QHBoxLayout;
    startGameBtn_ = new QPushButton("Start Game");
    stopGameBtn_ = new QPushButton("Stop");
    resetGameBtn_ = new QPushButton("Reset");
    connect(startGameBtn_, &QPushButton::clicked, this, &ExperimentalTab::onStartGame);
    connect(stopGameBtn_, &QPushButton::clicked, this, &ExperimentalTab::onStopGame);
    connect(resetGameBtn_, &QPushButton::clicked, this, &ExperimentalTab::onResetGame);
    gameButtons->addWidget(startGameBtn_);
    gameButtons->addWidget(stopGameBtn_);
    gameButtons->addWidget(resetGameBtn_);
    gameLayout->addLayout(gameButtons);

    gameStatusLabel_ = new QLabel("Idle");
    gameStatusLabel_->setProperty("role", "status");
    gameLayout->addWidget(gameStatusLabel_);
    gameView_ = new BlockGameView;
    gameView_->setGame(&blockGame_);
    gameLayout->addWidget(gameView_);
    layout->addWidget(gameGroup);

    layout->addStretch();

    // Periodic writer for the binary export
    writeTimer_ = new QTimer(this);
    writeTimer_->setInterval(33);
    connect(writeTimer_, &QTimer::timeout, this, &ExperimentalTab::writeBinaryFrame);

    gameTimer_ = new QTimer(this);
    gameTimer_->setInterval(33);
    connect(gameTimer_, &QTimer::timeout, this, &ExperimentalTab::updateGameFrame);
    gameTimer_->start();
}

ExperimentalTab::~ExperimentalTab() {
    if (angleBinFile_) { fclose(angleBinFile_); angleBinFile_ = nullptr; }
}

bool ExperimentalTab::binaryExportEnabled() const {
    return binaryExportCb_ && binaryExportCb_->isChecked();
}

bool ExperimentalTab::legacyPlaybackEnabled() const {
    return legacyPlaybackCb_ && legacyPlaybackCb_->isChecked();
}

bool ExperimentalTab::resampledPlaybackEnabled() const {
    return resampledPlaybackCb_ && resampledPlaybackCb_->isChecked();
}

void ExperimentalTab::onBinaryExportToggled(bool checked) {
    if (checked) {
        const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
        if (desktop.isEmpty()) {
            QMessageBox::warning(this, "Error", "Could not determine desktop directory.");
            binaryExportCb_->setChecked(false);
            return;
        }
        angleBinPath_ = QDir(desktop).filePath("knee_flexion.bin").toStdString();
        angleBinFile_ = fopen(angleBinPath_.c_str(), "wb");
        if (!angleBinFile_) {
            QMessageBox::warning(this, "Error", "Failed to open binary angle export file.");
            binaryExportCb_->setChecked(false);
            return;
        }
        binaryStatusLabel_->setText("<span style='color:#4ce070;'>ACTIVE</span>");
        binaryPathLabel_->setText(QString("File: %1").arg(QString::fromStdString(angleBinPath_)));
        writeTimer_->start();
    } else {
        writeTimer_->stop();
        if (angleBinFile_) { fclose(angleBinFile_); angleBinFile_ = nullptr; }
        binaryStatusLabel_->clear();
        binaryPathLabel_->clear();
    }
}

void ExperimentalTab::writeBinaryFrame() {
    if (!angleBinFile_) return;
    std::shared_ptr<FrameData> framePtr;
    {
        auto& slot = ctrl_->slot(0);
        std::lock_guard<std::mutex> lock(slot.frameMutex);
        framePtr = slot.sharedFrame;
    }
    if (!framePtr || framePtr->bodies.empty()) return;

    struct { float knee; float confidence; } binData = {};
    auto& body = framePtr->bodies[0];
    const auto& kneeDef = getAngleDefinition(BiomechAngle::RIGHT_KNEE_FLEXION);
    auto& p = body.joints[kneeDef.proximalJoint];
    auto& v = body.joints[kneeDef.vertexJoint];
    auto& d = body.joints[kneeDef.distalJoint];
    float avgConf = (p.confidence + v.confidence + d.confidence) / 3.0f;
    if (avgConf >= 0.1f) {
        float inc = BiofeedbackEngine::computeAngle(p, v, d);
        binData.knee = ((float)M_PI - inc) * 180.0f / (float)M_PI;
        binData.confidence = avgConf;
    }
    fseek(angleBinFile_, 0, SEEK_SET);
    fwrite(&binData, sizeof(binData), 1, angleBinFile_);
    fflush(angleBinFile_);
}

void ExperimentalTab::onStartGame() {
    if (!blockGameCb_->isChecked()) blockGameCb_->setChecked(true);
    blockGame_.start();
    gameClock_.restart();
    lastGameMs_ = 0;
    updateGameFrame();
}

void ExperimentalTab::onStopGame() {
    blockGame_.stop();
    updateGameFrame();
}

void ExperimentalTab::onResetGame() {
    blockGame_.reset();
    gameClock_.restart();
    lastGameMs_ = 0;
    updateGameFrame();
}

void ExperimentalTab::updateGameFrame() {
    if (!blockGameCb_ || !blockGameCb_->isChecked()) {
        if (gameView_) gameView_->update();
        return;
    }

    if (!gameClock_.isValid()) gameClock_.start();
    qint64 now = gameClock_.elapsed();
    float dt = lastGameMs_ > 0 ? (now - lastGameMs_) / 1000.0f : 0.033f;
    lastGameMs_ = now;

    std::shared_ptr<FrameData> framePtr;
    {
        auto& slot = ctrl_->slot(0);
        std::lock_guard<std::mutex> lock(slot.frameMutex);
        framePtr = slot.sharedFrame;
    }
    if (framePtr) blockGame_.update(dt, *framePtr);

    QString state = "Idle";
    if (blockGame_.state() == GameState::Playing) state = "Playing";
    else if (blockGame_.state() == GameState::GameOver) state = "Game Over";
    gameStatusLabel_->setText(QString("%1 | Score: %2 | Lives: %3 | Time: %4 s")
                                  .arg(state)
                                  .arg(blockGame_.score())
                                  .arg(blockGame_.lives())
                                  .arg(blockGame_.elapsedTime(), 0, 'f', 1));
    gameView_->update();
}
