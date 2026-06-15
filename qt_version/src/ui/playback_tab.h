#pragma once
#include <QWidget>
#include <QElapsedTimer>

class AppController;
class ExperimentalTab;
class SkeletonView;
class QPushButton;
class QLineEdit;
class QLabel;
class QSlider;
class QComboBox;
class QDoubleSpinBox;
class QRadioButton;
class QCheckBox;
class QWidget;

class PlaybackTab : public QWidget {
    Q_OBJECT
public:
    PlaybackTab(AppController* ctrl, ExperimentalTab* exp, QWidget* parent = nullptr);

private slots:
    void onBrowse1();
    void onLoadFile1();
    void onBrowse2();
    void onLoadFile2();
    void onLoadLegacy1();
    void onLoadLegacy2();
    void onLoadResampled1();
    void onLoadResampled2();
    void onPlayPause();
    void onReset();
    void onFrameBack();
    void onFrameForward();
    void onCompareModeChanged();
    void onSeek(int value);
    void onViewportFrameSwapped();
    void onLegacyToggled(bool);
    void onResampledToggled(bool);

private:
    void updateFrame(bool updateInfo = true);
    void updateSeekBar();
    void updateInfoPanel();

    AppController* ctrl_;
    ExperimentalTab* exp_;

    SkeletonView* viewMain_ = nullptr;
    SkeletonView* viewSecondary_ = nullptr;

    // File 1
    QLineEdit* path1Edit_ = nullptr;
    QLabel* file1InfoLabel_ = nullptr;
    QPushButton* browse1Btn_ = nullptr;
    QPushButton* load1Btn_ = nullptr;
    QPushButton* loadLegacy1Btn_ = nullptr;
    QPushButton* loadResampled1Btn_ = nullptr;

    // File 2
    QLineEdit* path2Edit_ = nullptr;
    QLabel* file2InfoLabel_ = nullptr;
    QPushButton* browse2Btn_ = nullptr;
    QPushButton* load2Btn_ = nullptr;
    QPushButton* loadLegacy2Btn_ = nullptr;
    QPushButton* loadResampled2Btn_ = nullptr;
    QWidget* file2Section_ = nullptr;
    QWidget* syncSection_ = nullptr;

    // Comparison mode
    QRadioButton* modeSingleRb_ = nullptr;
    QRadioButton* modeSplitRb_ = nullptr;
    QRadioButton* modeOverlayRb_ = nullptr;
    QDoubleSpinBox* timeOffsetSpin_ = nullptr;

    // Playback controls
    QPushButton* playBtn_ = nullptr;
    QPushButton* resetBtn_ = nullptr;
    QPushButton* frameBackBtn_ = nullptr;
    QPushButton* frameFwdBtn_ = nullptr;
    QSlider* speedSlider_ = nullptr;
    QLabel* speedValueLabel_ = nullptr;

    QLabel* timeLabel_ = nullptr;
    QSlider* seekSlider_ = nullptr;
    QLabel* durationLabel_ = nullptr;
    QLabel* infoLabel_ = nullptr;

    bool playing_ = false;
    double playbackTime_ = 0;
    qint64 lastPlaybackNsec_ = 0;
    qint64 lastControlsRefreshNsec_ = 0;
    float playbackSpeed_ = 1.0f;
    QElapsedTimer playbackClock_;
};
