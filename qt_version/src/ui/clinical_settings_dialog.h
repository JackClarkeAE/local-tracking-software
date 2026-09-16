#pragma once
#include <QDialog>

class AppController;
class QComboBox;
class QLineEdit;

// Clinical defaults: what the Record tab pre-selects on start-up. Opened
// from the gear button beside the tabs. Values are written to config.ini
// under [defaults] and applied to the Record tab immediately on save.
class ClinicalSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ClinicalSettingsDialog(AppController* ctrl, QWidget* parent = nullptr);

private slots:
    void onCameraTypeChanged();
    void onSave();

private:
    void populateDevices();
    void populateModels();
    void populateProtocols();
    // Select the entry matching `text`; if it's a saved default that isn't
    // currently available (e.g. a camera that's unplugged), keep it as an
    // extra entry so saving doesn't silently drop it.
    static void selectOrAdd(QComboBox* combo, const QString& text);

    AppController* ctrl_;
    QComboBox* cameraTypeCombo_ = nullptr;
    QComboBox* deviceCombo_ = nullptr;
    QComboBox* modelCombo_ = nullptr;
    QWidget* modelRow_ = nullptr;
    QComboBox* protocolCombo_ = nullptr;
    QLineEdit* clinicianEdit_ = nullptr;
};
