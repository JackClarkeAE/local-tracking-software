#include "config_setup_dialog.h"
#include "../app_controller.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QFileDialog>

ConfigSetupDialog::ConfigSetupDialog(AppController* ctrl, QWidget* parent)
    : QDialog(parent), ctrl_(ctrl) {
    setWindowTitle("Data Directory Setup");
    setModal(true);
    setMinimumWidth(550);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    messageLabel_ = new QLabel(QString::fromStdString(ctrl_->configSetupMessage()));
    messageLabel_->setWordWrap(true);
    layout->addWidget(messageLabel_);

    auto addPathRow = [&](const QString& title, QLineEdit*& edit, auto browseSlot) {
        layout->addWidget(new QLabel(title));
        auto* row = new QHBoxLayout;
        edit = new QLineEdit;
        auto* browseBtn = new QPushButton("Browse…");
        connect(browseBtn, &QPushButton::clicked, this, browseSlot);
        connect(edit, &QLineEdit::textChanged, this, [this] { updateSaveEnabled(); });
        row->addWidget(edit, 1);
        row->addWidget(browseBtn);
        layout->addLayout(row);
    };
    addPathRow("Recordings Directory:", recordingsEdit_, &ConfigSetupDialog::onBrowseRecordings);
    addPathRow("Protocols Directory:", protocolsEdit_, &ConfigSetupDialog::onBrowseProtocols);

    recordingsEdit_->setText(QString::fromStdString(ctrl_->config().recordingsDir));
    protocolsEdit_->setText(QString::fromStdString(ctrl_->config().protocolsDir));

    auto* footer = new QHBoxLayout;
    saveBtn_ = new QPushButton("Save");
    saveBtn_->setDefault(true);
    connect(saveBtn_, &QPushButton::clicked, this, &ConfigSetupDialog::onSave);
    auto* hint = new QLabel("Directories will be created if they don't exist.");
    hint->setStyleSheet("color: #888;");
    footer->addWidget(saveBtn_);
    footer->addWidget(hint, 1);
    layout->addLayout(footer);

    updateSaveEnabled();
}

void ConfigSetupDialog::updateSaveEnabled() {
    // textChanged fires while the constructor is still populating fields,
    // before the Save button exists
    if (!saveBtn_) return;
    saveBtn_->setEnabled(!recordingsEdit_->text().trimmed().isEmpty() &&
                         !protocolsEdit_->text().trimmed().isEmpty());
}

void ConfigSetupDialog::onBrowseRecordings() {
    const QString picked = QFileDialog::getExistingDirectory(
        this, "Select Recordings Directory", recordingsEdit_->text());
    if (!picked.isEmpty()) recordingsEdit_->setText(picked);
}

void ConfigSetupDialog::onBrowseProtocols() {
    const QString picked = QFileDialog::getExistingDirectory(
        this, "Select Protocols Directory", protocolsEdit_->text());
    if (!picked.isEmpty()) protocolsEdit_->setText(picked);
}

void ConfigSetupDialog::onSave() {
    std::string error;
    if (ctrl_->applyDataDirectories(recordingsEdit_->text().trimmed().toStdString(),
                                    protocolsEdit_->text().trimmed().toStdString(),
                                    &error)) {
        accept();
    } else {
        messageLabel_->setText(QString("<span style='color:#e05050;'>%1</span>")
                               .arg(QString::fromStdString(error)));
    }
}
