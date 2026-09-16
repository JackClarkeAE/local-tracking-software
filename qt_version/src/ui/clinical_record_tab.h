#pragma once
#include <QWidget>

class AppController;
class SkeletonView;
class PopoutWindow;
class CameraFeedView;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QLabel;
class QCheckBox;
class QComboBox;
class QTimer;
class QProgressBar;
class QFrame;

// Clinical UI layer: the Record tab.
//
// A reduced version of the research Live tab for use in clinic. It keeps the
// same two views — the patient-facing screen and the clinician's view — and
// the same controller, but exposes only what a session needs: who the
// patient is, which camera to use and which assessment protocol to run.
// Recording is driven by the protocol's own events and named after the
// patient. Camera tuning, dual-camera, overlays, smoothing and biofeedback
// stay in the research UI.
class ClinicalRecordTab : public QWidget {
    Q_OBJECT
public:
    explicit ClinicalRecordTab(AppController* ctrl, QWidget* parent = nullptr);
    ~ClinicalRecordTab() override;

private slots:
    void onTick();
    void onCameraCategoryChanged();
    void onCameraSelectionChanged();
    void onStartCamera();
    void onStopCamera();
    void onRunAssessment();
    void onStopAssessment();
    void onPopOut();
    void onSessionStateChanged(int state);

private:
    void applyCameraSettings();
    void populateDeviceCombo();
    void populateModelCombo();
    void refreshProtocolList();
    void updateButtons();
    void updateStatus();
    void updateProtocolUI();
    void updateViewLayout();

    AppController* ctrl_;

    // Views
    SkeletonView* patientScreen_ = nullptr;
    SkeletonView* clinicianView_ = nullptr;
    CameraFeedView* feedView_ = nullptr;
    QWidget* patientScreenPanel_ = nullptr;
    QWidget* clinicianViewPanel_ = nullptr;
    QWidget* feedPanel_ = nullptr;
    QLabel* recordingBanner_ = nullptr;
    PopoutWindow* popout_ = nullptr;
    bool rgbCamerasPresent_ = false;

    // Status
    QLabel* statusPill_ = nullptr;
    QLabel* sessionInfoLabel_ = nullptr;

    // Patient
    QLineEdit* patientIdEdit_ = nullptr;
    QLineEdit* clinicianEdit_ = nullptr;
    QPlainTextEdit* notesEdit_ = nullptr;

    // Camera
    QComboBox* cameraCategoryCombo_ = nullptr;
    QComboBox* cameraDeviceCombo_ = nullptr;
    QComboBox* rgbModelCombo_ = nullptr;
    QWidget* rgbModelRow_ = nullptr;
    QPushButton* startCamBtn_ = nullptr;
    QPushButton* stopCamBtn_ = nullptr;

    // Assessment (protocol)
    QComboBox* protocolCombo_ = nullptr;
    QPushButton* runProtocolBtn_ = nullptr;
    QPushButton* stopProtocolBtn_ = nullptr;
    QLabel* protocolInfoLabel_ = nullptr;
    QProgressBar* protocolProgress_ = nullptr;
    QFrame* currentBox_ = nullptr;
    QFrame* nextBox_ = nullptr;
    QLabel* currentEventLabel_ = nullptr;
    QLabel* nextEventLabel_ = nullptr;
    QLabel* countdownLabel_ = nullptr;

    // Patient screen
    QCheckBox* patientScreenCb_ = nullptr;
    QPushButton* popOutBtn_ = nullptr;

    QTimer* tickTimer_ = nullptr;
};
