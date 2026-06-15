#pragma once
#include <QDialog>

class AppController;
class QLineEdit;
class QLabel;
class QPushButton;

// "Data Directory Setup" — shown on first launch (no config.ini) or when the
// configured directories fail validation. Mirrors the original ImGui app's
// modal: recordings + protocols paths with Browse buttons, Save disabled
// until both are set, directories created on save.
class ConfigSetupDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConfigSetupDialog(AppController* ctrl, QWidget* parent = nullptr);

private slots:
    void onBrowseRecordings();
    void onBrowseProtocols();
    void onSave();

private:
    void updateSaveEnabled();

    AppController* ctrl_;
    QLabel* messageLabel_ = nullptr;
    QLineEdit* recordingsEdit_ = nullptr;
    QLineEdit* protocolsEdit_ = nullptr;
    QPushButton* saveBtn_ = nullptr;
};
