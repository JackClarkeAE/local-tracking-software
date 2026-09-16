#include "clinical_settings_dialog.h"
#include "widget_kit.h"
#include "../app_controller.h"
#include "../config.h"
#include "../camera/model_tracker.h"
#include "../recording/protocol.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QMessageBox>
#include <QMediaDevices>
#include <QCameraDevice>
#include <set>

namespace {
const char* kNoneEntry = "(none)";
}

ClinicalSettingsDialog::ClinicalSettingsDialog(AppController* ctrl, QWidget* parent)
    : QDialog(parent), ctrl_(ctrl) {
    setWindowTitle("Defaults");
    setModal(true);
    setMinimumWidth(460);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);

    auto* intro = new QLabel(
        "What the Record tab selects when the app starts. "
        "Leave an entry blank to keep the built-in behaviour.");
    intro->setWordWrap(true);
    intro->setProperty("muted", true);
    layout->addWidget(intro);

    // One form so the label column lines up across sections; section
    // headers are spanning rows.
    auto* form = new QFormLayout;
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addLayout(form);

    form->addRow(WidgetKit::sectionHeader("Camera"));
    cameraTypeCombo_ = new QComboBox;
    cameraTypeCombo_->addItem("Depth camera (skeleton)", "depth");
    cameraTypeCombo_->addItem("RGB camera", "rgb");
    connect(cameraTypeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ClinicalSettingsDialog::onCameraTypeChanged);
    form->addRow("Type", cameraTypeCombo_);

    deviceCombo_ = new QComboBox;
    form->addRow("Device", deviceCombo_);

    modelCombo_ = new QComboBox;
    modelCombo_->setToolTip("Pose model used to track the patient on an RGB camera.");
    auto* modelLabel = new QLabel("Tracking");
    form->addRow(modelLabel, modelCombo_);
    modelRow_ = modelLabel;

    form->addRow(WidgetKit::sectionHeader("Assessment"));
    protocolCombo_ = new QComboBox;
    form->addRow("Protocol", protocolCombo_);

    form->addRow(WidgetKit::sectionHeader("Session"));
    clinicianEdit_ = new QLineEdit;
    clinicianEdit_->setPlaceholderText("Pre-filled Clinician name or initials");
    form->addRow("Clinician", clinicianEdit_);

    layout->addSpacing(6);
    auto* footer = new QHBoxLayout;
    auto* cancelBtn = WidgetKit::button("Cancel");
    auto* saveBtn = WidgetKit::button("Save", WidgetKit::ButtonRole::Primary);
    saveBtn->setDefault(true);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, this, &ClinicalSettingsDialog::onSave);
    footer->addStretch();
    footer->addWidget(cancelBtn);
    footer->addWidget(saveBtn);
    layout->addLayout(footer);

    // Load current values.
    const AppConfig& cfg = ctrl_->config();
    const int typeIdx = cameraTypeCombo_->findData(QString::fromStdString(cfg.defaultCameraType));
    cameraTypeCombo_->setCurrentIndex(typeIdx >= 0 ? typeIdx : 0);
    populateDevices();
    populateModels();
    populateProtocols();
    selectOrAdd(deviceCombo_, QString::fromStdString(cfg.defaultDevice));
    selectOrAdd(modelCombo_, QString::fromStdString(cfg.defaultRgbModel));
    selectOrAdd(protocolCombo_, QString::fromStdString(cfg.defaultProtocol));
    clinicianEdit_->setText(QString::fromStdString(cfg.defaultClinician));
    const bool rgb = cameraTypeCombo_->currentData().toString() == "rgb";
    modelRow_->setVisible(rgb);
    modelCombo_->setVisible(rgb);
}

void ClinicalSettingsDialog::onCameraTypeChanged() {
    const bool rgb = cameraTypeCombo_->currentData().toString() == "rgb";
    populateDevices();
    modelRow_->setVisible(rgb);
    modelCombo_->setVisible(rgb);
}

void ClinicalSettingsDialog::populateDevices() {
    // Keep the current choice across a repopulate (raw name for a
    // "(not available)" entry, otherwise the visible text).
    const QVariant raw = deviceCombo_->currentData(Qt::UserRole);
    const QString previous = raw.isValid() ? raw.toString() : deviceCombo_->currentText();
    deviceCombo_->blockSignals(true);
    deviceCombo_->clear();
    deviceCombo_->addItem(kNoneEntry);
    if (cameraTypeCombo_->currentData().toString() == "rgb") {
        for (const auto& d : QMediaDevices::videoInputs())
            deviceCombo_->addItem(d.description());
    } else {
        deviceCombo_->addItems({"ZED 2i", "Azure Kinect"});
    }
    deviceCombo_->blockSignals(false);
    if (!previous.isEmpty() && previous != kNoneEntry) selectOrAdd(deviceCombo_, previous);
}

void ClinicalSettingsDialog::populateModels() {
    modelCombo_->clear();
    modelCombo_->addItem(kNoneEntry);
    modelCombo_->addItem("None (video only)");
    std::set<std::string> seen;
    auto addFrom = [&](const std::string& dir) {
        for (const auto& m : listRgbModels(dir)) {
            if (!seen.insert(m.name).second) continue;
            modelCombo_->addItem(QString::fromStdString(m.name));
        }
    };
    addFrom(getBundledModelsDir());
    addFrom(ctrl_->config().rgbModelsDir);
}

void ClinicalSettingsDialog::populateProtocols() {
    protocolCombo_->clear();
    protocolCombo_->addItem(kNoneEntry);
    for (const auto& f : listProtocolFiles(ctrl_->config().protocolsDir))
        protocolCombo_->addItem(QString::fromStdString(f));
}

void ClinicalSettingsDialog::selectOrAdd(QComboBox* combo, const QString& text) {
    if (text.isEmpty()) { combo->setCurrentIndex(0); return; }
    int idx = combo->findText(text);
    if (idx < 0) {
        combo->addItem(text + "  (not available)");
        idx = combo->count() - 1;
        combo->setItemData(idx, text, Qt::UserRole);
    }
    combo->setCurrentIndex(idx);
}

void ClinicalSettingsDialog::onSave() {
    auto value = [](QComboBox* combo) -> std::string {
        if (combo->currentIndex() <= 0) return "";
        const QVariant raw = combo->currentData(Qt::UserRole);
        const QString text = raw.isValid() ? raw.toString() : combo->currentText();
        return text.toStdString();
    };

    AppConfig& cfg = ctrl_->config();
    cfg.defaultCameraType = cameraTypeCombo_->currentData().toString().toStdString();
    cfg.defaultDevice = value(deviceCombo_);
    cfg.defaultRgbModel = cfg.defaultCameraType == "rgb" ? value(modelCombo_) : "";
    cfg.defaultProtocol = value(protocolCombo_);
    cfg.defaultClinician = clinicianEdit_->text().trimmed().toStdString();

    if (!saveConfig(getConfigPath(), cfg)) {
        QMessageBox::warning(this, "Defaults",
            QString("Failed to write %1").arg(QString::fromStdString(getConfigPath())));
        return;
    }
    accept();
}
