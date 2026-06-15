#pragma once
#include "config.h"
#include "camera/icamera.h"
#include "camera/camera_types.h"
#include "recording/joint_recorder.h"
#include "recording/playback.h"
#include "recording/protocol.h"
#include "recording/flag_recorder.h"
#include "data/session_scanner.h"
#include "data/data_exporter.h"
#include "biofeedback/biofeedback_engine.h"
#include "biofeedback/biofeedback_recorder.h"
#include "ui/overlay_state.h"

#include <QObject>
#include <QTimer>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <string>
#include <chrono>
#include <map>
#include <array>

static constexpr int MAX_CAMERAS = 2;

enum class SessionState { Stopped, Running, Paused };
enum class CompareMode { Single, SplitScreen, Overlay };

struct DebugInfo {
    float cameraFps[MAX_CAMERAS] = {};
    int bodyCount[MAX_CAMERAS] = {};
    bool isRecording = false;
    uint64_t droppedFrames = 0;
    float sessionSeconds = 0.0f;
    int activeCameras = 0;
};

struct CameraSlot {
    std::unique_ptr<ICamera> camera;
    std::shared_ptr<FrameData> sharedFrame = std::make_shared<FrameData>();
    std::mutex frameMutex;
    std::thread trackingThread;
    std::atomic<bool> trackingRunning{false};
    std::atomic<bool> trackingPaused{false};
    JointRecorder recorder;
    std::atomic<float> cameraFps{0.0f};
    std::atomic<int> bodyCount{0};
    std::map<uint32_t, std::array<Joint, JOINT_COUNT>> prevJoints;
    std::map<uint32_t, std::array<Joint2D, JOINT_COUNT>> prevJoints2D;
    bool enabled = false;
    std::atomic<bool> cameraDisconnected{false};
};

// AppController centralizes all non-UI state and logic.
// Each Qt UI tab receives a reference to the controller and emits/receives
// signals to/from it.
class AppController : public QObject {
    Q_OBJECT
public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    void init();
    void shutdown();

    // Camera
    void startCamera(int slotIndex);
    void stopCamera(int slotIndex);
    void pauseTracking();
    void resumeTracking();
    void togglePause();
    bool isAnyRecording() const;
    bool isDualCamera() const;

    CameraType cameraType(int slotIndex) const { return cameraTypes_[slotIndex]; }
    void setCameraType(int slotIndex, CameraType type);
    CameraConfig cameraConfig(int slotIndex) const { return cameraConfigs_[slotIndex]; }
    void setCameraConfig(int slotIndex, const CameraConfig& config);

    // Recording
    void startRecording();
    void stopRecording();
    void writeSessionMetadata();

    // Protocol
    void executeProtocolAction(const ProtocolAction& action);
    void loadProtocol(const Protocol& p);
    void unloadProtocol();
    void runProtocol();
    void abortProtocol();
    bool isProtocolLoaded() const { return protocolLoaded_; }

    // Accessors
    AppConfig& config() { return config_; }
    const AppConfig& config() const { return config_; }
    SessionState sessionState() const { return sessionState_; }
    CameraSlot& slot(int i) { return slots_[i]; }
    const CameraSlot& slot(int i) const { return slots_[i]; }
    ProtocolRunner& protocolRunner() { return protocolRunner_; }
    const Protocol& loadedProtocol() const { return loadedProtocol_; }
    BiofeedbackEngine& biofeedbackEngine() { return biofeedbackEngine_; }
    bool biofeedbackActive() const { return biofeedbackActive_; }
    bool avatarVisible() const { return avatarVisible_; }
    void setAvatarVisible(bool v) { avatarVisible_ = v; }
    PlaybackManager& playback() { return playback_; }
    PlaybackManager& compPlayback() { return compPlayback_; }
    SessionScanner& sessionScanner() { return sessionScanner_; }
    DataExporter& dataExporter() { return dataExporter_; }

    // SDK availability
    bool zedSdkAvailable() const { return zedSdkAvailable_; }
    bool kinectSdkAvailable() const { return kinectSdkAvailable_; }
    const std::string& sdkWarning() const { return sdkWarningMessage_; }

    // First-run / invalid-config setup (Data Directory Setup dialog)
    bool configSetupNeeded() const { return configSetupNeeded_; }
    const std::string& configSetupMessage() const { return configSetupMessage_; }
    // Creates the directories, validates, persists config.ini and emits
    // configChanged()/protocolsChanged(). Returns false with *error set on failure.
    bool applyDataDirectories(const std::string& recordingsDir,
                              const std::string& protocolsDir,
                              std::string* error = nullptr);
    void notifyProtocolsChanged() { emit protocolsChanged(); }

    // Session metadata
    std::string patientId;
    std::string operatorName;
    std::string sessionNotes;
    std::string recordingFileName = "session";

    // Countdown
    bool countdownEnabled = false;
    int countdownSeconds = 3;
    float goDisplaySeconds = 2.0f;

    // Recording
    bool recordJoints = false;
    bool recordVideo = false;  // also record camera footage to MP4
    bool videoRecordingSupported() const;

    // Smoothing
    float smoothingFactor = 0.0f;

    // Debug info (updated by timer)
    DebugInfo debugInfo;

    // Protocol timing
    std::chrono::steady_clock::time_point sessionStart;
    std::chrono::steady_clock::time_point recordingStart;
    std::chrono::steady_clock::time_point protocolStartTime;

    // Countdown state
    bool countdownActive = false;
    int countdownRemaining = 0;
    std::chrono::steady_clock::time_point countdownStart;
    bool goDisplayActive = false;
    std::chrono::steady_clock::time_point goDisplayStartTime;

    // Comparison mode
    CompareMode compareMode = CompareMode::Single;
    double compTimeOffset = 0.0;

    // Live display state
    bool showModifiedInViewer = false;
    bool showJointAngles = false;

    const OverlayState& overlayState() const { return overlayState_; }
    void setOverlayText(const std::string& text);
    void setOverlayTextFull(const std::string& text, TextPosition pos,
                            float scale, const QColor& color);
    void setOverlayShape(OverlayShape shape);
    void clearOverlay();

    // Audio cue. Accepts a builtin sound name (beep/chime/alert) or a
    // path to a local WAV file.
    void playSound(const std::string& soundParam);

    // Paths helpers
    std::string buildRecordingPath(int slotIndex = 0);
    std::string buildFlagPath();
    std::string buildBiofeedbackAvatarPath();
    std::string buildBiofeedbackAnglesPath();
    std::string buildVideoPath(int slotIndex = 0);

    // Frame processing
    void applySmoothingToFrame(FrameData& frame, int slotIndex);

signals:
    void sessionStateChanged(int state);
    void cameraStarted(int slotIndex);
    void cameraStopped(int slotIndex);
    void recordingStarted();
    void recordingStopped();
    void errorOccurred(QString message);
    void frameReady(int slotIndex);  // emitted periodically when new frame data is available
    void protocolActionFired(QString eventName);
    void overlayChanged();
    void displayOptionsChanged();
    void configChanged();
    void protocolsChanged();

private slots:
    void onTick();  // 30 FPS timer for debug info + protocol + countdown updates

private:
    void trackingLoop(int slotIndex);
    void checkSdkAvailability();
    void parseBiofeedbackTransformParam(const std::string& param);

    AppConfig config_;
    SessionState sessionState_ = SessionState::Stopped;
    CameraSlot slots_[MAX_CAMERAS];
    CameraType cameraTypes_[MAX_CAMERAS] = { CameraType::ZED2i, CameraType::AzureKinect };
    CameraConfig cameraConfigs_[MAX_CAMERAS];

    FlagRecorder flagRecorder_;

    // Camera footage recording (one per camera slot)
    class VideoRecorder* videoRecorders_[MAX_CAMERAS] = {};
    uint64_t lastVideoFrameTs_[MAX_CAMERAS] = {};

    // Protocol
    ProtocolRunner protocolRunner_;
    Protocol loadedProtocol_;
    bool protocolLoaded_ = false;

    // Playback
    PlaybackManager playback_;
    PlaybackManager compPlayback_;

    // Data
    SessionScanner sessionScanner_;
    DataExporter dataExporter_;

    // Biofeedback
    BiofeedbackEngine biofeedbackEngine_;
    BiofeedbackRecorder biofeedbackRecorder_;
    bool biofeedbackActive_ = false;
    bool avatarVisible_ = true;

    // SDK availability
    bool zedSdkAvailable_ = false;
    bool kinectSdkAvailable_ = false;
    std::string sdkWarningMessage_;

    // Config setup state
    bool configSetupNeeded_ = false;
    std::string configSetupMessage_;
    OverlayState overlayState_;

    // Tick timer
    QTimer* tickTimer_ = nullptr;

    // Audio cue player (lazily created QSoundEffect)
    QObject* soundEffect_ = nullptr;
};
