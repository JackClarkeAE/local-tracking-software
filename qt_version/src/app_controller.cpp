#include "app_controller.h"
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <fstream>
#include <iostream>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
// windows.h (mmsystem) defines a PlaySound -> PlaySoundW macro that collides
// with ProtocolEventType::PlaySound. We don't use the Win32 audio API here.
#ifdef PlaySound
#undef PlaySound
#endif
#elif defined(__linux__)
#include <dlfcn.h>
#endif

#include <QStandardPaths>
#include <QDir>
#include <QSoundEffect>
#include <QUrl>
#include <QFile>
#include <QGuiApplication>
#if QT_CONFIG(permissions)
#include <QPermissions>
#endif
#include "recording/video_recorder.h"
#include "camera/model_tracker.h"

AppController::AppController(QObject* parent) : QObject(parent) {
    cameraConfigs_[0].deviceIndex = 0;
    cameraConfigs_[1].deviceIndex = 1;

    tickTimer_ = new QTimer(this);
    tickTimer_->setInterval(33);  // ~30 FPS
    connect(tickTimer_, &QTimer::timeout, this, &AppController::onTick);
}

AppController::~AppController() {
    shutdown();
}

void AppController::init() {
    std::string iniPath = getConfigPath();
    if (!loadConfig(iniPath, config_)) {
        // First launch — suggest writable defaults (next to the exe for a
        // portable build, else the user's Documents) and confirm via the
        // Data Directory Setup dialog
        const std::string root = getDefaultDataRoot();
        config_.recordingsDir = root + "/recordings";
        config_.protocolsDir = root + "/protocols";
        configSetupNeeded_ = true;
        configSetupMessage_ = "Welcome! Please select your data directories.";
    } else {
        const std::string warning = validateConfig(config_);
        if (!warning.empty()) {
            configSetupNeeded_ = true;
            configSetupMessage_ = warning;
        }
    }
    // Drop-in RGB tracking models live alongside the data directories;
    // create the folder so users can see where models go
    if (config_.rgbModelsDir.empty())
        config_.rgbModelsDir = defaultRgbModelsDir(config_.recordingsDir);
    if (!config_.rgbModelsDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(config_.rgbModelsDir, ec);
    }

    checkSdkAvailability();
    tickTimer_->start();
}

void AppController::shutdown() {
    if (tickTimer_) tickTimer_->stop();

    // Stop all cameras and recording
    for (int i = 0; i < MAX_CAMERAS; ++i) stopCamera(i);

    stopRecording();

    if (biofeedbackRecorder_.isRecording()) biofeedbackRecorder_.stop();
}

void AppController::checkSdkAvailability() {
#ifdef _WIN32
    HMODULE h = LoadLibraryA("sl_zed64.dll");
    zedSdkAvailable_ = (h != nullptr);
    if (h) FreeLibrary(h);

    h = LoadLibraryA("k4a.dll");
    kinectSdkAvailable_ = (h != nullptr);
    if (h) FreeLibrary(h);
#elif defined(__linux__)
    zedSdkAvailable_ = false;
    kinectSdkAvailable_ = false;
#ifdef HAS_ZED_SDK
    if (void* zh = dlopen("libsl_zed.so", RTLD_LAZY | RTLD_NOLOAD)) {
        zedSdkAvailable_ = true;
        dlclose(zh);
    } else if (void* zh2 = dlopen("libsl_zed.so", RTLD_LAZY)) {
        zedSdkAvailable_ = true;
        dlclose(zh2);
    }
#endif
#ifdef HAS_K4A_SDK
    if (void* kh = dlopen("libk4a.so", RTLD_LAZY | RTLD_NOLOAD)) {
        kinectSdkAvailable_ = true;
        dlclose(kh);
    } else if (void* kh2 = dlopen("libk4a.so.1.4", RTLD_LAZY)) {
        kinectSdkAvailable_ = true;
        dlclose(kh2);
    }
#endif
#else
    // macOS: no vendor camera SDKs exist — playback/analysis only
    zedSdkAvailable_ = false;
    kinectSdkAvailable_ = false;
#endif

    if (!zedSdkAvailable_ && !kinectSdkAvailable_) {
        sdkWarningMessage_ = "No camera SDKs found. The application will run in playback-only mode.";
    } else if (!zedSdkAvailable_) {
        sdkWarningMessage_ = "ZED SDK not available (sl_zed64.dll missing).";
    } else if (!kinectSdkAvailable_) {
        sdkWarningMessage_ = "Azure Kinect SDK not available (k4a.dll missing).";
    }
}

bool AppController::applyDataDirectories(const std::string& recordingsDir,
                                         const std::string& protocolsDir,
                                         std::string* error) {
    AppConfig candidate;
    candidate.recordingsDir = recordingsDir;
    candidate.protocolsDir = protocolsDir;

    std::error_code ec;
    std::filesystem::create_directories(recordingsDir, ec);
    std::filesystem::create_directories(protocolsDir, ec);

    const std::string validation = validateConfig(candidate);
    if (!validation.empty()) {
        if (error) *error = validation;
        return false;
    }
    config_ = candidate;
    if (!saveConfig(getConfigPath(), config_)) {
        if (error) *error = "Failed to write config.ini at " + getConfigPath();
        return false;
    }
    configSetupNeeded_ = false;
    configSetupMessage_.clear();
    emit configChanged();
    emit protocolsChanged();
    return true;
}

bool AppController::isAnyRecording() const {
    for (int i = 0; i < MAX_CAMERAS; ++i) {
        if (slots_[i].recorder.isRecording()) return true;
    }
    return false;
}

bool AppController::isDualCamera() const {
    return slots_[0].enabled && slots_[1].enabled;
}

void AppController::setCameraType(int slotIndex, CameraType type) {
    if (slotIndex < 0 || slotIndex >= MAX_CAMERAS) return;
    cameraTypes_[slotIndex] = type;
}

void AppController::setCameraConfig(int slotIndex, const CameraConfig& config) {
    if (slotIndex < 0 || slotIndex >= MAX_CAMERAS) return;
    cameraConfigs_[slotIndex] = config;
    cameraConfigs_[slotIndex].deviceIndex = slotIndex;
}

void AppController::pauseTracking() {
    if (sessionState_ == SessionState::Stopped) return;
    for (int i = 0; i < MAX_CAMERAS; ++i) {
        if (slots_[i].enabled) slots_[i].trackingPaused.store(true);
    }
    sessionState_ = SessionState::Paused;
    emit sessionStateChanged((int)sessionState_);
}

void AppController::resumeTracking() {
    if (sessionState_ == SessionState::Stopped) return;
    for (int i = 0; i < MAX_CAMERAS; ++i) {
        if (slots_[i].enabled) slots_[i].trackingPaused.store(false);
    }
    sessionState_ = SessionState::Running;
    emit sessionStateChanged((int)sessionState_);
}

void AppController::togglePause() {
    if (sessionState_ == SessionState::Paused) resumeTracking();
    else if (sessionState_ == SessionState::Running) pauseTracking();
}

void AppController::startCamera(int slotIndex) {
    if (slotIndex < 0 || slotIndex >= MAX_CAMERAS) return;
    auto& slot = slots_[slotIndex];
    if (slot.camera && slot.camera->isOpen()) return;

    CameraConfig cfg = cameraConfigs_[slotIndex];
    CameraType camType = cameraTypes_[slotIndex];

    // Device index is the ordinal among slots using the SAME camera type —
    // each vendor SDK enumerates only its own devices, so a mixed pair
    // (e.g. ZED + Kinect) must open each vendor's device #0.
    int vendorOrdinal = 0;
    for (int i = 0; i < slotIndex; ++i) {
        if (cameraTypes_[i] == camType) vendorOrdinal++;
    }
    cfg.deviceIndex = vendorOrdinal;
    if (camType == CameraType::ZED2i && !zedSdkAvailable_) {
        emit errorOccurred(QStringLiteral("ZED SDK is not available. Ensure CUDA and ZED SDK DLLs are installed."));
        return;
    }
    if (camType == CameraType::AzureKinect && !kinectSdkAvailable_) {
        emit errorOccurred(QStringLiteral("Azure Kinect SDK is not available. Ensure K4A and K4ABT DLLs are installed."));
        return;
    }
#if QT_CONFIG(permissions)
    if (camType == CameraType::RGBWebcam) {
        // macOS requires explicit camera permission. Ask on first use and
        // retry the start automatically once the user grants it.
        QCameraPermission cameraPermission;
        switch (qApp->checkPermission(cameraPermission)) {
            case Qt::PermissionStatus::Undetermined:
                qApp->requestPermission(cameraPermission, this,
                    [this, slotIndex](const QPermission& p) {
                        if (p.status() == Qt::PermissionStatus::Granted) {
                            startCamera(slotIndex);
                        } else {
                            emit errorOccurred(QStringLiteral(
                                "Camera access was denied. Enable it in System Settings → Privacy & Security → Camera."));
                        }
                    });
                return;
            case Qt::PermissionStatus::Denied:
                emit errorOccurred(QStringLiteral(
                    "Camera access is denied. Enable it in System Settings → Privacy & Security → Camera, then try again."));
                return;
            case Qt::PermissionStatus::Granted:
                break;
        }
    }
#endif

    if (camType == CameraType::RGBWebcam && !cfg.rgbModelOnnx.empty()) {
        // RGB camera with a drop-in tracking model behaves like a
        // markerless camera: frames carry both image and tracked bodies
        slot.camera = std::make_unique<ModelTracker>(createCamera(camType));
    } else {
        slot.camera = createCamera(camType);
    }

    if (slot.camera && slot.camera->open(cfg)) {
        slot.enabled = true;
        slot.trackingPaused.store(false);
        slot.trackingRunning.store(true);
        slot.prevJoints.clear();
        slot.prevJoints2D.clear();
        slot.cameraDisconnected.store(false);
        slot.trackingThread = std::thread(&AppController::trackingLoop, this, slotIndex);

        if (slotIndex == 0) {
            sessionState_ = SessionState::Running;
            sessionStart = std::chrono::steady_clock::now();
        }
        emit cameraStarted(slotIndex);
        emit sessionStateChanged((int)sessionState_);
    } else {
        emit errorOccurred(QString("Failed to open camera %1. Check connection.").arg(slotIndex + 1));
        slot.camera.reset();
    }
}

void AppController::stopCamera(int slotIndex) {
    if (slotIndex < 0 || slotIndex >= MAX_CAMERAS) return;
    auto& slot = slots_[slotIndex];
    if (!slot.camera) return;

    slot.trackingRunning.store(false);
    slot.trackingPaused.store(false);
    if (slot.trackingThread.joinable()) slot.trackingThread.join();
    slot.recorder.stop();
    slot.camera->close();
    slot.camera.reset();
    slot.enabled = false;
    slot.prevJoints.clear();
    slot.prevJoints2D.clear();
    {
        std::lock_guard<std::mutex> lock(slot.frameMutex);
        slot.sharedFrame = std::make_shared<FrameData>();
    }

    if (slotIndex == 0) {
        sessionState_ = SessionState::Stopped;
        avatarVisible_ = true;
        emit sessionStateChanged((int)sessionState_);
    }
    emit cameraStopped(slotIndex);
}

void AppController::setOverlayText(const std::string& text) {
    overlayState_.text = text;
    emit overlayChanged();
}

void AppController::setOverlayTextFull(const std::string& text, TextPosition pos,
                                       float scale, const QColor& color) {
    overlayState_.text = text;
    overlayState_.textPosition = pos;
    overlayState_.textScale = std::clamp(scale, 0.5f, 5.0f);
    overlayState_.textColor = color;
    emit overlayChanged();
}

void AppController::setOverlayShape(OverlayShape shape) {
    overlayState_.shape = shape;
    emit overlayChanged();
}

void AppController::clearOverlay() {
    overlayState_ = OverlayState{};
    emit overlayChanged();
}

void AppController::trackingLoop(int slotIndex) {
    auto& slot = slots_[slotIndex];
    auto lastTime = std::chrono::steady_clock::now();
    int camFrameCount = 0;
    int consecutiveFailures = 0;
    while (slot.trackingRunning.load(std::memory_order_acquire)) {
        if (slot.trackingPaused.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        auto frame = std::make_shared<FrameData>();
        if (slot.camera && slot.camera->grabFrame(*frame)) {
            consecutiveFailures = 0;
            camFrameCount++;
            auto now = std::chrono::steady_clock::now();
            float elapsed = std::chrono::duration<float>(now - lastTime).count();
            if (elapsed >= 0.5f) {
                slot.cameraFps.store(camFrameCount / elapsed, std::memory_order_relaxed);
                camFrameCount = 0;
                lastTime = now;
            }
            slot.bodyCount.store((int)frame->bodies.size(), std::memory_order_relaxed);
            if (slot.recorder.isRecording()) slot.recorder.recordFrame(*frame);
            { std::lock_guard<std::mutex> lock(slot.frameMutex); slot.sharedFrame = frame; }
        } else {
            consecutiveFailures++;
            if (consecutiveFailures >= 30) {
                slot.cameraDisconnected.store(true, std::memory_order_release);
                break;
            }
        }
    }
}

void AppController::startRecording() {
    // One shared epoch for the whole session: both cameras' joint CSVs,
    // both footage MP4s and the flags CSV all measure time from this
    // instant, so multi-camera data aligns directly in analysis.
    recordingStart = std::chrono::steady_clock::now();

    for (int i = 0; i < MAX_CAMERAS; ++i) {
        if (slots_[i].enabled && slots_[i].camera && slots_[i].camera->isOpen()) {
            if (recordJoints && !slots_[i].recorder.isRecording()) {
                slots_[i].recorder.start(buildRecordingPath(i), recordingStart);
            }
            if (recordVideo) {
                if (!VideoRecorder::supported()) {
                    emit errorOccurred(QStringLiteral(
                        "Camera footage recording requires Qt 6.6 or newer — joints will still be recorded."));
                } else {
                    if (!videoRecorders_[i]) {
                        videoRecorders_[i] = new VideoRecorder(this);
                        connect(videoRecorders_[i], &VideoRecorder::recordingError,
                                this, &AppController::errorOccurred);
                    }
                    if (!videoRecorders_[i]->isRecording()) {
                        lastVideoFrameTs_[i] = 0;
                        videoRecorders_[i]->start(buildVideoPath(i), recordingStart);
                    }
                }
            }
        }
    }
    flagRecorder_.start(buildFlagPath());
    writeSessionMetadata();
    emit recordingStarted();
}

void AppController::stopRecording() {
    for (int i = 0; i < MAX_CAMERAS; ++i) {
        slots_[i].recorder.stop();
        if (videoRecorders_[i]) videoRecorders_[i]->stop();
    }
    flagRecorder_.stop();
    emit recordingStopped();
}

bool AppController::videoRecordingSupported() const {
    return VideoRecorder::supported();
}

std::string AppController::buildRecordingPath(int slotIndex) {
    std::filesystem::create_directories(config_.recordingsDir);
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << config_.recordingsDir << "/" << recordingFileName;
    if (isDualCamera()) oss << "_cam" << (slotIndex + 1);
    oss << "_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".csv";
    return oss.str();
}

std::string AppController::buildFlagPath() {
    std::filesystem::create_directories(config_.recordingsDir);
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << config_.recordingsDir << "/" << recordingFileName
        << "_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << "_flags.csv";
    return oss.str();
}

std::string AppController::buildVideoPath(int slotIndex) {
    std::filesystem::create_directories(config_.recordingsDir);
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << config_.recordingsDir << "/" << recordingFileName;
    if (isDualCamera()) oss << "_cam" << (slotIndex + 1);
    oss << "_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << "_video.mp4";
    return oss.str();
}

std::string AppController::buildBiofeedbackAvatarPath() {
    std::filesystem::create_directories(config_.recordingsDir);
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << config_.recordingsDir << "/" << recordingFileName
        << "_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << "_biofeedback_avatar.csv";
    return oss.str();
}

std::string AppController::buildBiofeedbackAnglesPath() {
    std::filesystem::create_directories(config_.recordingsDir);
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << config_.recordingsDir << "/" << recordingFileName
        << "_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << "_biofeedback_angles.csv";
    return oss.str();
}

void AppController::writeSessionMetadata() {
    std::filesystem::create_directories(config_.recordingsDir);
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream pathOss;
    pathOss << config_.recordingsDir << "/" << recordingFileName
            << "_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << "_metadata.json";

    std::ofstream f(pathOss.str());
    if (!f.is_open()) return;

    auto esc = [](const std::string& s) -> std::string {
        std::string o;
        for (char c : s) {
            if (c == '"') o += "\\\"";
            else if (c == '\\') o += "\\\\";
            else if (c == '\n') o += "\\n";
            else o += c;
        }
        return o;
    };

    f << "{\n";
    f << "  \"patient_id\": \"" << esc(patientId) << "\",\n";
    f << "  \"operator\": \"" << esc(operatorName) << "\",\n";
    f << "  \"notes\": \"" << esc(sessionNotes) << "\",\n";

    char dateBuf[32];
    strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d %H:%M:%S", &tm);
    f << "  \"date\": \"" << dateBuf << "\",\n";
    f << "  \"cameras\": [\n";
    bool first = true;
    for (int i = 0; i < MAX_CAMERAS; ++i) {
        if (slots_[i].enabled && slots_[i].camera) {
            if (!first) f << ",\n";
            f << "    { \"slot\": " << i << ", \"name\": \"" << slots_[i].camera->cameraName() << "\" }";
            first = false;
        }
    }
    f << "\n  ],\n";
    if (protocolLoaded_) {
        f << "  \"protocol\": \"" << esc(loadedProtocol_.name) << "\",\n";
    }
    f << "  \"biofeedback_active\": " << (biofeedbackActive_ ? "true" : "false") << ",\n";
    f << "  \"video_recorded\": " << (recordVideo ? "true" : "false") << "\n";
    f << "}\n";
}

void AppController::applySmoothingToFrame(FrameData& frame, int slotIndex) {
    if (slotIndex < 0 || slotIndex >= MAX_CAMERAS) return;
    float s = smoothingFactor;
    if (s <= 0.0f) return;
    auto& slot = slots_[slotIndex];

    for (auto& body : frame.bodies) {
        auto it = slot.prevJoints.find(body.id);
        if (it != slot.prevJoints.end()) {
            auto& prev = it->second;
            for (int j = 0; j < JOINT_COUNT; ++j) {
                if (body.joints[j].confidence < 0.1f) continue;
                if (prev[j].confidence < 0.1f) { prev[j] = body.joints[j]; continue; }
                body.joints[j].x = s * prev[j].x + (1.0f - s) * body.joints[j].x;
                body.joints[j].y = s * prev[j].y + (1.0f - s) * body.joints[j].y;
                body.joints[j].z = s * prev[j].z + (1.0f - s) * body.joints[j].z;
                prev[j] = body.joints[j];
            }
        } else {
            slot.prevJoints[body.id] = body.joints;
        }

        auto it2d = slot.prevJoints2D.find(body.id);
        if (it2d != slot.prevJoints2D.end()) {
            auto& prev2d = it2d->second;
            for (int j = 0; j < JOINT_COUNT; ++j) {
                if (!body.joints2D[j].valid) continue;
                if (!prev2d[j].valid) { prev2d[j] = body.joints2D[j]; continue; }
                body.joints2D[j].x = s * prev2d[j].x + (1.0f - s) * body.joints2D[j].x;
                body.joints2D[j].y = s * prev2d[j].y + (1.0f - s) * body.joints2D[j].y;
                prev2d[j] = body.joints2D[j];
            }
        } else {
            slot.prevJoints2D[body.id] = body.joints2D;
        }
    }
}

void AppController::executeProtocolAction(const ProtocolAction& action) {
    switch (action.type) {
        case ProtocolEventType::StartCamera:
            startCamera(0);
            emit protocolActionFired("Start Camera");
            break;
        case ProtocolEventType::Countdown: {
            int secs = 3;
            float goTime = 2.0f;
            auto comma = action.parameter.find(',');
            if (comma != std::string::npos) {
                try { secs = std::stoi(action.parameter.substr(0, comma)); } catch (...) {}
                try { goTime = std::stof(action.parameter.substr(comma + 1)); } catch (...) {}
            } else {
                try { secs = std::stoi(action.parameter); } catch (...) {}
            }
            countdownActive = true;
            countdownRemaining = secs;
            goDisplaySeconds = goTime;
            goDisplayActive = false;
            countdownStart = std::chrono::steady_clock::now();
            emit protocolActionFired(QString("Countdown %1s").arg(secs));
            break;
        }
        case ProtocolEventType::StartRecording:
            startRecording();
            emit protocolActionFired("Start Recording");
            break;
        case ProtocolEventType::StopRecording:
            stopRecording();
            emit protocolActionFired("Stop Recording");
            break;
        case ProtocolEventType::AddFlag: {
            if (flagRecorder_.isActive()) {
                auto elapsed = std::chrono::steady_clock::now() - recordingStart;
                double secs = std::chrono::duration<double>(elapsed).count();
                flagRecorder_.addFlag(secs, action.parameter);
            }
            emit protocolActionFired(QString("Flag: %1").arg(QString::fromStdString(action.parameter)));
            break;
        }
        case ProtocolEventType::DisplayText: {
            std::string text = action.parameter;
            TextPosition pos = TextPosition::Top;
            float scale = 1.0f;
            QColor color(255, 255, 255);

            auto sep1 = action.parameter.find('|');
            if (sep1 != std::string::npos) {
                text = action.parameter.substr(0, sep1);
                std::string rest = action.parameter.substr(sep1 + 1);

                auto sep2 = rest.find('|');
                std::string posStr = (sep2 != std::string::npos) ? rest.substr(0, sep2) : rest;
                if (posStr == "middle") pos = TextPosition::Middle;
                else if (posStr == "bottom") pos = TextPosition::Bottom;

                if (sep2 != std::string::npos) {
                    rest = rest.substr(sep2 + 1);
                    auto sep3 = rest.find('|');
                    std::string scaleStr = (sep3 != std::string::npos) ? rest.substr(0, sep3) : rest;
                    try { scale = std::stof(scaleStr); } catch (...) {}

                    if (sep3 != std::string::npos) {
                        std::string colStr = rest.substr(sep3 + 1);
                        if (colStr == "red") color = QColor(255, 80, 80);
                        else if (colStr == "green") color = QColor(80, 230, 80);
                        else if (colStr == "blue") color = QColor(80, 130, 255);
                        else if (colStr == "yellow") color = QColor(255, 230, 50);
                        else if (colStr == "cyan") color = QColor(80, 230, 230);
                    }
                }
            }
            setOverlayTextFull(text, pos, scale, color);
            emit protocolActionFired(QString("Display Text"));
            break;
        }
        case ProtocolEventType::DisplayShape:
            if (action.parameter == "circle") setOverlayShape(OverlayShape::RedCircle);
            else if (action.parameter == "square") setOverlayShape(OverlayShape::BlueSquare);
            else if (action.parameter == "triangle") setOverlayShape(OverlayShape::GreenTriangle);
            emit protocolActionFired(QString("Display Shape"));
            break;
        case ProtocolEventType::ClearOverlay:
            clearOverlay();
            emit protocolActionFired("Clear Overlay");
            break;
        case ProtocolEventType::StopCamera:
            stopRecording();
            stopCamera(0);
            emit protocolActionFired("Stop Camera");
            break;
        case ProtocolEventType::HideAvatar:
            avatarVisible_ = false;
            emit protocolActionFired("Hide Avatar");
            break;
        case ProtocolEventType::ShowAvatar:
            avatarVisible_ = true;
            emit protocolActionFired("Show Avatar");
            break;
        case ProtocolEventType::BeginBiofeedback:
            biofeedbackEngine_.clearTransforms();
            biofeedbackRecorder_.start(buildBiofeedbackAvatarPath(), buildBiofeedbackAnglesPath());
            biofeedbackActive_ = true;
            emit protocolActionFired("Begin Biofeedback");
            break;
        case ProtocolEventType::EndBiofeedback:
            biofeedbackEngine_.clearTransforms();
            biofeedbackRecorder_.stop();
            biofeedbackActive_ = false;
            emit protocolActionFired("End Biofeedback");
            break;
        case ProtocolEventType::SetBiofeedbackTransform:
            parseBiofeedbackTransformParam(action.parameter);
            emit protocolActionFired("Set BF Transform");
            break;
        case ProtocolEventType::ClearBiofeedbackTransform:
            if (action.parameter == "ALL") {
                biofeedbackEngine_.clearTransforms();
            } else {
                int idx = biomechAngleFromName(action.parameter);
                if (idx >= 0) biofeedbackEngine_.removeTransform((BiomechAngle)idx);
            }
            emit protocolActionFired("Clear BF Transform");
            break;
        case ProtocolEventType::PlaySound:
            playSound(action.parameter);
            emit protocolActionFired(QString("Play Sound: %1")
                .arg(QString::fromStdString(action.parameter)));
            break;
    }
}

void AppController::playSound(const std::string& soundParam) {
    if (soundParam.empty()) return;

    QUrl source;
    if (soundParam == "beep" || soundParam == "chime" || soundParam == "alert") {
        source = QUrl(QString("qrc:/ui/sounds/%1.wav").arg(QString::fromStdString(soundParam)));
    } else {
        const QString path = QString::fromStdString(soundParam);
        if (!QFile::exists(path)) {
            emit errorOccurred(QString("Sound file not found: %1").arg(path));
            return;
        }
        source = QUrl::fromLocalFile(path);
    }

    if (!soundEffect_) soundEffect_ = new QSoundEffect(this);
    auto* effect = static_cast<QSoundEffect*>(soundEffect_);
    if (effect->source() != source) effect->setSource(source);
    effect->setVolume(1.0);
    effect->play();
}

void AppController::parseBiofeedbackTransformParam(const std::string& param) {
    std::istringstream ss(param);
    std::string angleName;
    float startScale = 1.0f, endScale = 1.0f, rampStart = 0.0f, rampDuration = 0.0f;

    std::getline(ss, angleName, ',');
    std::string tok;
    if (std::getline(ss, tok, ',')) { try { startScale = std::stof(tok); } catch (...) {} }
    if (std::getline(ss, tok, ',')) { try { endScale = std::stof(tok); } catch (...) {} }
    if (std::getline(ss, tok, ',')) { try { rampStart = std::stof(tok); } catch (...) {} }
    if (std::getline(ss, tok, ',')) { try { rampDuration = std::stof(tok); } catch (...) {} }

    int idx = biomechAngleFromName(angleName);
    if (idx >= 0) {
        float protoTime = (protocolRunner_.state() == ProtocolRunnerState::Running)
            ? protocolRunner_.currentTime() : 0.0f;
        biofeedbackEngine_.setTransform((BiomechAngle)idx, startScale, endScale,
                                         protoTime + rampStart, rampDuration);
    }
}

void AppController::loadProtocol(const Protocol& p) {
    loadedProtocol_ = p;
    protocolLoaded_ = true;
    protocolRunner_.reset();

    // Auto-tick recording if protocol contains StartRecording
    for (auto& ev : loadedProtocol_.events) {
        if (ev.type == ProtocolEventType::StartRecording) {
            recordJoints = true;
            break;
        }
    }
}

void AppController::unloadProtocol() {
    protocolRunner_.reset();
    loadedProtocol_ = Protocol{};
    protocolLoaded_ = false;
    recordJoints = false;
    clearOverlay();
}

void AppController::runProtocol() {
    if (!protocolLoaded_) return;
    protocolRunner_.load(loadedProtocol_);
    protocolRunner_.start();
    protocolStartTime = std::chrono::steady_clock::now();
}

void AppController::abortProtocol() {
    protocolRunner_.abort();
    stopRecording();
    stopCamera(0);
    stopCamera(1);
    countdownActive = false;
}

void AppController::onTick() {
    // Check for camera disconnections
    for (int i = 0; i < MAX_CAMERAS; ++i) {
        if (slots_[i].enabled && slots_[i].cameraDisconnected.load(std::memory_order_acquire)) {
            slots_[i].cameraDisconnected.store(false);
            std::string camName = slots_[i].camera ? slots_[i].camera->cameraName() : "Camera";
            emit errorOccurred(QString("%1 (slot %2) disconnected. Recording preserved.")
                               .arg(QString::fromStdString(camName)).arg(i + 1));
            stopRecording();
            stopCamera(i);
        }
    }

    // Update debug info
    debugInfo.activeCameras = 0;
    debugInfo.droppedFrames = 0;
    debugInfo.isRecording = isAnyRecording();
    for (int i = 0; i < MAX_CAMERAS; ++i) {
        debugInfo.cameraFps[i] = slots_[i].cameraFps.load(std::memory_order_relaxed);
        debugInfo.bodyCount[i] = slots_[i].bodyCount.load(std::memory_order_relaxed);
        debugInfo.droppedFrames += slots_[i].recorder.droppedFrames();
        if (slots_[i].enabled) debugInfo.activeCameras++;
    }
    if (sessionState_ != SessionState::Stopped) {
        auto elapsed = std::chrono::steady_clock::now() - sessionStart;
        debugInfo.sessionSeconds = std::chrono::duration<float>(elapsed).count();
    }

    // Protocol runner update
    if (protocolRunner_.state() == ProtocolRunnerState::Running) {
        auto elapsed = std::chrono::steady_clock::now() - protocolStartTime;
        float secs = std::chrono::duration<float>(elapsed).count();
        auto actions = protocolRunner_.update(secs);
        for (auto& a : actions) executeProtocolAction(a);
    }

    // Countdown state machine
    if (countdownActive) {
        auto elapsed = std::chrono::steady_clock::now() - countdownStart;
        int elapsedSec = (int)std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        int remaining = countdownRemaining - elapsedSec;
        if (remaining <= 0) {
            countdownActive = false;
            goDisplayActive = true;
            goDisplayStartTime = std::chrono::steady_clock::now();
            if (protocolRunner_.state() != ProtocolRunnerState::Running) {
                startRecording();
            }
        }
    }
    if (goDisplayActive) {
        auto elapsed = std::chrono::steady_clock::now() - goDisplayStartTime;
        float secs = std::chrono::duration<float>(elapsed).count();
        if (secs >= goDisplaySeconds) goDisplayActive = false;
    }

    // Camera footage recording: push the latest frame of each slot to its
    // video encoder (deduplicated by camera timestamp)
    for (int i = 0; i < MAX_CAMERAS; ++i) {
        if (!videoRecorders_[i] || !videoRecorders_[i]->isRecording()) continue;
        std::shared_ptr<FrameData> framePtr;
        { std::lock_guard<std::mutex> lock(slots_[i].frameMutex); framePtr = slots_[i].sharedFrame; }
        if (framePtr && framePtr->timestamp_us != lastVideoFrameTs_[i]) {
            lastVideoFrameTs_[i] = framePtr->timestamp_us;
            videoRecorders_[i]->pushFrame(*framePtr);
        }
    }

    // Biofeedback recording: if active, apply transforms and record
    if (biofeedbackActive_ && biofeedbackEngine_.hasActiveTransforms() && biofeedbackRecorder_.isRecording()) {
        std::shared_ptr<FrameData> framePtr;
        { std::lock_guard<std::mutex> lock(slots_[0].frameMutex); framePtr = slots_[0].sharedFrame; }
        if (framePtr && !framePtr->bodies.empty()) {
            float protoTime = (protocolRunner_.state() == ProtocolRunnerState::Running)
                ? protocolRunner_.currentTime() : debugInfo.sessionSeconds;
            FrameData modified = biofeedbackEngine_.applyTransforms(*framePtr, protoTime);
            auto sessionElapsed = std::chrono::steady_clock::now() - sessionStart;
            double sessionSecs = std::chrono::duration<double>(sessionElapsed).count();
            biofeedbackRecorder_.recordFrame(modified, biofeedbackEngine_.lastMeasurements(), sessionSecs);
        }
    }

    // Signal frame-ready for both slots so UI tabs can refresh
    for (int i = 0; i < MAX_CAMERAS; ++i) {
        if (slots_[i].enabled) emit frameReady(i);
    }
}
