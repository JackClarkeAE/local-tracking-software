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
#include <limits>
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
    // offsetGuess: raw time = avatar time + offset; NaN = search the whole session
    bool loadRecordedAvatar(const QString& path,
                            double offsetGuess = std::numeric_limits<double>::quiet_NaN());

    // Programmatic control (also used by UI)
    void setSettings(const anat::Settings& settings);
    const anat::Settings& settings() const { return settings_; }
    void setEngine(Engine engine);
    void setSelectedLimb(int side);
    void seekToTime(double seconds);
    void setCameraPreset(const QString& preset);  // "front", "left", "right", "top"
    void recomputeNow();                          // skip the debounce (scripting / tests)

    // A companion file (_flags, _biofeedback_avatar, _biofeedback_angles,
    // _metadata) picked in the dialog resolves to its joint recording
    static QString resolveSessionPath(const QString& picked);

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void hideEvent(QHideEvent* e) override;

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
    anat::Settings settingsAt(int frame) const;  // per-frame forward sign, temporal limit
    void applyFloorMode();
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
    anat::FloorPlane floor_;            // in use (fitted, or level per the Floor control)
    anat::FloorPlane floorFitted_;
    anat::FloorPlane floorLevel_;       // camera Y, at the lowest contact height
    int floorMode_ = 0;                 // 0 auto (fitted), 1 camera Y
    std::vector<int> frameForward_;     // per-frame forward sign (2D data can turn around)
    bool facingFlips3D_ = false;        // 3D data whose facing seems to flip (tracker L/R swaps)
    int gapsFilled_ = 0;
    int bodySwitches_ = 0;              // frames where the subject's tracking id changed
    bf::Vec3 lastTarget_;
    bool haveLastTarget_ = false;
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
    bool fillGaps_ = true;
    bool smoothVertical_ = true;
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
    QWidget* transport_ = nullptr;

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
    QCheckBox* fillGapsCb_ = nullptr;
    QCheckBox* smoothVerticalCb_ = nullptr;
    QCheckBox* pelvisDropCb_ = nullptr;
    QComboBox* hipRefCombo_ = nullptr;
    QComboBox* forwardCombo_ = nullptr;
    QComboBox* floorCombo_ = nullptr;
    QToolButton* ghostBtn_ = nullptr;
    QToolButton* axesBtn_ = nullptr;
    QToolButton* labelsBtn_ = nullptr;

    QLabel* readout_ = nullptr;
};
