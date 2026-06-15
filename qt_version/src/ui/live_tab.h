#pragma once
#include <QWidget>

class AppController;
class SkeletonView;
class PopoutWindow;
class QLineEdit;
class QPushButton;
class QLabel;
class QCheckBox;
class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
class QTimer;
class QProgressBar;
class QFrame;
class CameraFeedWindow;
class QRadioButton;

class LiveTab : public QWidget {
    Q_OBJECT
public:
    explicit LiveTab(AppController* ctrl, QWidget* parent = nullptr);
    ~LiveTab() override;

private slots:
    void onTick();
    void onCameraCategoryChanged();
    void onStartCamera();
    void onStopCamera();
    void onPauseResume();
    void onStartRecording();
    void onStopRecording();
    void onStartCamera2();
    void onStopCamera2();
    void onPopOut();
    void onLoadProtocol();
    void onUnloadProtocol();
    void onRunProtocol();
    void onAbortProtocol();
    void onProtocolActionFired(QString name);
    void onSessionStateChanged(int state);
    void onCameraSettingsChanged();
    void onCamera2SettingsChanged();
    void onOverlayChanged();
    void onShowFeedToggled(bool enabled);

private:
    void refreshProtocolList();
    void updateButtons();
    void updateStatus();
    void updateProtocolUI();
    void updateCameraSettingVisibility();
    void applyCameraSettings(int slotIndex);
    void populateCameraDeviceCombo();
    void populateDeviceCombo(QComboBox* typeCombo, int category, int markerlessDefault);
    void populateModelCombo(QComboBox* modelCombo);
    void updateViewLayout();

    AppController* ctrl_;

    // Skeleton views
    SkeletonView* patientScreen_ = nullptr;
    SkeletonView* patientViewer_ = nullptr;
    QWidget* patientScreenPanel_ = nullptr;
    QWidget* patientViewerPanel_ = nullptr;

    // Inline camera feed (main view for RGB cameras)
    class CameraFeedView* feedView_ = nullptr;
    QWidget* feedPanel_ = nullptr;
    QCheckBox* patientScreenCb_ = nullptr;
    QCheckBox* recordVideoCb_ = nullptr;

    // Pop-out window
    PopoutWindow* popout_ = nullptr;

    // Session metadata
    QLineEdit* patientIdEdit_ = nullptr;
    QLineEdit* operatorEdit_ = nullptr;
    QLineEdit* notesEdit_ = nullptr;

    // Camera controls
    QComboBox* cameraCategoryCombo_ = nullptr;
    QComboBox* cameraTypeCombo_ = nullptr;
    QComboBox* rgbModelCombo_ = nullptr;
    QWidget* rgbModelRow_ = nullptr;
    bool rgbCamerasPresent_ = false;
    int feedSlot_ = 0;  // which slot the inline camera feed displays
    QWidget* zedSettingsGroup_ = nullptr;
    QWidget* kinectSettingsGroup_ = nullptr;
    QComboBox* zedBodyFormatCombo_ = nullptr;
    QComboBox* zedTrackingCombo_ = nullptr;
    QComboBox* zedResolutionCombo_ = nullptr;
    QComboBox* zedDepthCombo_ = nullptr;
    QComboBox* k4aProcessingCombo_ = nullptr;
    QComboBox* k4aDepthCombo_ = nullptr;
    QComboBox* k4aColorCombo_ = nullptr;
    QComboBox* k4aFpsCombo_ = nullptr;
    QComboBox* k4aOrientationCombo_ = nullptr;
    QPushButton* startCamBtn_ = nullptr;
    QPushButton* pauseBtn_ = nullptr;
    QPushButton* stopCamBtn_ = nullptr;

    // Recording
    QLineEdit* fileNameEdit_ = nullptr;
    QCheckBox* recordCb_ = nullptr;
    QPushButton* startRecBtn_ = nullptr;
    QPushButton* stopRecBtn_ = nullptr;
    QCheckBox* countdownCb_ = nullptr;
    QSpinBox* countdownSecs_ = nullptr;
    QDoubleSpinBox* goDisplaySecs_ = nullptr;

    // Camera 2
    QCheckBox* camera2Cb_ = nullptr;
    QWidget* camera2Details_ = nullptr;
    QComboBox* camera2CategoryCombo_ = nullptr;
    QComboBox* camera2TypeCombo_ = nullptr;
    QComboBox* rgbModel2Combo_ = nullptr;
    QWidget* rgbModel2Row_ = nullptr;
    QWidget* zed2SettingsGroup_ = nullptr;
    QWidget* kinect2SettingsGroup_ = nullptr;
    QComboBox* zed2BodyFormatCombo_ = nullptr;
    QComboBox* zed2TrackingCombo_ = nullptr;
    QComboBox* zed2ResolutionCombo_ = nullptr;
    QComboBox* zed2DepthCombo_ = nullptr;
    QComboBox* k4a2ProcessingCombo_ = nullptr;
    QComboBox* k4a2DepthCombo_ = nullptr;
    QComboBox* k4a2ColorCombo_ = nullptr;
    QComboBox* k4a2FpsCombo_ = nullptr;
    QComboBox* k4a2OrientationCombo_ = nullptr;
    QPushButton* startCam2Btn_ = nullptr;
    QPushButton* stopCam2Btn_ = nullptr;

    // Display
    QDoubleSpinBox* smoothingSlider_ = nullptr;
    QCheckBox* showAnglesCb_ = nullptr;
    QCheckBox* showFeedCb_ = nullptr;
    QCheckBox* showModifiedCb_ = nullptr;

    // Overlay
    QLineEdit* overlayTextEdit_ = nullptr;
    QComboBox* overlayPositionCombo_ = nullptr;
    QDoubleSpinBox* overlayScaleSpin_ = nullptr;
    QComboBox* overlayColorCombo_ = nullptr;
    QRadioButton* shapeNoneRb_ = nullptr;
    QRadioButton* shapeCircleRb_ = nullptr;
    QRadioButton* shapeSquareRb_ = nullptr;
    QRadioButton* shapeTriangleRb_ = nullptr;

    // Pop-out
    QPushButton* popOutBtn_ = nullptr;
    CameraFeedWindow* feedWindow_ = nullptr;
    QWidget* biofeedbackSection_ = nullptr;

    // Status
    QLabel* statusLabel_ = nullptr;
    QLabel* debugLabel_ = nullptr;

    // Protocol section
    QComboBox* protocolCombo_ = nullptr;
    QPushButton* loadProtocolBtn_ = nullptr;
    QPushButton* unloadProtocolBtn_ = nullptr;
    QPushButton* runProtocolBtn_ = nullptr;
    QPushButton* abortProtocolBtn_ = nullptr;
    QProgressBar* protocolProgress_ = nullptr;
    QLabel* currentEventLabel_ = nullptr;
    QLabel* nextEventLabel_ = nullptr;
    QLabel* countdownLabel_ = nullptr;
    QLabel* protocolInfoLabel_ = nullptr;
    QFrame* currentBox_ = nullptr;
    QFrame* nextBox_ = nullptr;

    QTimer* tickTimer_ = nullptr;
};
