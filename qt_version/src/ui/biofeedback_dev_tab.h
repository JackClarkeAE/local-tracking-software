#pragma once
// EXPERIMENTAL — Biofeedback Development tab.
//
// Replays a recorded session through a biofeedback engine so the transforms
// can be tuned offline. Laid out like a video editor: linked 3D viewports
// (original | modified) on top, a transport bar and curve timeline in the
// middle, and engine / limb controls at the bottom.
//
// Engines:
//   Anatomical (new)   — anat:: engine (signed sagittal angles, step length)
//   Current live engine — BiofeedbackEngine, as used on the patient screen
//   Recorded avatar     — the session's _biofeedback_avatar.csv, i.e. exactly
//                         what the patient was shown, auto-aligned in time

#include "../biofeedback/anatomical_engine.h"
#include "../biofeedback/biofeedback_engine.h"
#include "../recording/playback.h"

#include <QElapsedTimer>
#include <QString>
#include <QWidget>
#include <memory>
#include <vector>

class AppController;
class BiofeedbackDevView;
class BiofeedbackTimeline;
struct BfOrbitCamera;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QRadioButton;
class QSlider;
class QTimer;
class QToolButton;

class BiofeedbackDevTab : public QWidget {
    Q_OBJECT
public:
    enum class Engine { Anatomical = 0, Legacy = 1, Recorded = 2 };

    explicit BiofeedbackDevTab(AppController* ctrl, QWidget* parent = nullptr);
    ~BiofeedbackDevTab() override;

    bool loadSession(const QString& path);
    bool loadRecordedAvatar(const QString& path, double offsetGuess = -1.0);

    // Programmatic control (also used by UI)
    void setSettings(const anat::Settings& settings);
    const anat::Settings& settings() const { return settings_; }
    void setEngine(Engine engine);
    void setSelectedLimb(int side);
    void seekToTime(double seconds);
    void setCameraPreset(const QString& preset);  // "front", "left", "right", "top"

private:
    struct GainRow {
        QWidget* row = nullptr;
        QSlider* slider = nullptr;
        QDoubleSpinBox* spin = nullptr;
    };

    QWidget* buildToolbar();
    QWidget* buildTransport();
    QWidget* buildControls();
    GainRow makeGainRow(const QString& label, const QString& tip, float anat::LimbSettings::*field);

    void rebuildInput();      // smoothing
    void scheduleRecompute();
    void recompute();         // run the engine over every frame
    void rebuildTracks();
    void refreshFrame();
    void refreshReadout();
    void syncInspector();
    void syncLimbList();
    void syncEngineCombo();
    void updateForwardSign();
    void autoDetectRecordedAvatar();
    double alignRecordedAvatar(double guess, double* matchErrorMm);
    const TrackedBody* recordedBodyAt(double rawTime) const;

    void setPlaying(bool playing);
    void stepFrames(int delta);
    void onTick();
    void setCurrentFrame(int index);
    int frameIndexAt(double t) const;

    AppController* ctrl_ = nullptr;

    // Session
    QString sessionPath_;
    PlaybackManager session_;
    std::vector<double> times_;
    std::vector<TrackedBody> raw_;      // primary body per frame
    std::vector<char> hasBody_;
    std::vector<TrackedBody> input_;    // after smoothing
    std::vector<TrackedBody> output_;
    std::vector<char> outputMissing_;   // recorded avatar has no frame here
    std::vector<anat::BodyDiagnostics> diag_;
    bool planar_ = false;
    bool hasFloor_ = false;
    float floorY_ = 0.0f;
    bf::Vec3 sessionCentre_;
    bf::Vec3 sessionAnt_{0, 0, -1}, sessionMl_{-1, 0, 0};
    int detectedForward_ = 0;
    uint32_t primaryBodyId_ = 0;

    // Recorded biofeedback avatar
    PlaybackManager avatar_;
    bool avatarLoaded_ = false;
    double avatarOffset_ = 0.0;   // raw time = avatar time + offset
    QString avatarPath_;

    // Engine state
    anat::Settings settings_;
    BiofeedbackEngine legacy_;
    Engine engine_ = Engine::Anatomical;
    int forwardMode_ = 0;          // 0 auto, 1 normal, 2 flipped
    float smoothing_ = 0.0f;
    int selectedLimb_ = 0;

    // Playback
    int cur_ = 0;
    bool playing_ = false;
    bool loop_ = true;
    double playTime_ = 0.0;
    float speed_ = 1.0f;
    QElapsedTimer clock_;
    qint64 lastTickNs_ = 0;
    QTimer* playTimer_ = nullptr;
    QTimer* recomputeTimer_ = nullptr;

    // View state
    std::shared_ptr<BfOrbitCamera> camA_, camB_;
    bool linked_ = true;
    bool follow_ = true;
    bool showGhost_ = true;
    bool showAxes_ = false;
    bool showLabels_ = true;

    // Widgets
    BiofeedbackDevView* viewOriginal_ = nullptr;
    BiofeedbackDevView* viewModified_ = nullptr;
    BiofeedbackTimeline* timeline_ = nullptr;
    QLabel* sessionLabel_ = nullptr;
    QLabel* dataNoteLabel_ = nullptr;
    QToolButton* linkBtn_ = nullptr;
    QToolButton* followBtn_ = nullptr;

    QToolButton* playBtn_ = nullptr;
    QToolButton* loopBtn_ = nullptr;
    QComboBox* speedCombo_ = nullptr;
    QLabel* timecodeLabel_ = nullptr;
    QLabel* durationLabel_ = nullptr;
    QLabel* frameLabel_ = nullptr;

    QListWidget* limbList_ = nullptr;
    QLabel* inspectorTitle_ = nullptr;
    QRadioButton* modeAnglesRb_ = nullptr;
    QRadioButton* modeStepRb_ = nullptr;
    GainRow hipRow_, kneeRow_, ankleRow_, stepRow_;
    QLabel* neutralLabel_ = nullptr;

    QComboBox* engineCombo_ = nullptr;
    QLabel* avatarStatus_ = nullptr;
    QDoubleSpinBox* smoothingSpin_ = nullptr;
    QCheckBox* groundCb_ = nullptr;
    QCheckBox* pelvisDropCb_ = nullptr;
    QComboBox* hipRefCombo_ = nullptr;
    QComboBox* forwardCombo_ = nullptr;
    QCheckBox* ghostCb_ = nullptr;
    QCheckBox* axesCb_ = nullptr;
    QCheckBox* labelsCb_ = nullptr;

    QLabel* readout_ = nullptr;
};
