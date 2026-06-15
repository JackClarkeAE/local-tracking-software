#include "protocol_editor_tab.h"
#include "../app_controller.h"
#include "../biofeedback/biofeedback_engine.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QLineEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QLabel>
#include <QListWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QStringList>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

static std::string defaultParameterFor(ProtocolEventType type) {
    switch (type) {
        case ProtocolEventType::Countdown: return "3,2.0";
        case ProtocolEventType::AddFlag: return "Flag";
        case ProtocolEventType::DisplayText: return "Text|top|1.0|white";
        case ProtocolEventType::DisplayShape: return "circle";
        case ProtocolEventType::SetBiofeedbackTransform:
            return std::string(biomechAngleName(0)) + ",1.00,1.00,0.0,0.0";
        case ProtocolEventType::ClearBiofeedbackTransform: return "ALL";
        case ProtocolEventType::PlaySound: return "beep";
        default: return "";
    }
}

ProtocolEditorTab::ProtocolEditorTab(AppController* ctrl, QWidget* parent)
    : QWidget(parent), ctrl_(ctrl) {
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setHandleWidth(5);
    root->addWidget(splitter);

    // Left: event table
    auto* leftPane = new QWidget;
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(6);
    leftLayout->addWidget(new QLabel("<h3>Protocol Events</h3>"));

    auto* relativeCb = new QCheckBox("Show time relative to previous event");
    relativeCb->setToolTip("Display and edit each event's time as a delay after\n"
                           "the previous event instead of absolute session time.");
    connect(relativeCb, &QCheckBox::toggled, this, [this](bool checked) {
        relativeTime_ = checked;
        rebuildEventTable();
    });
    leftLayout->addWidget(relativeCb);

    table_ = new QTableWidget;
    table_->setColumnCount(5);
    table_->setHorizontalHeaderLabels({"Time (s)", "Event", "Parameter", "Order", "Actions"});
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(34);
    table_->verticalHeader()->setMinimumSectionSize(32);
    table_->setColumnWidth(0, 88);
    table_->setColumnWidth(1, 180);
    table_->setColumnWidth(3, 82);
    table_->setColumnWidth(4, 82);
    leftLayout->addWidget(table_);
    splitter->addWidget(leftPane);

    // Right: controls
    auto* rightPane = new QWidget;
    auto* rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(8, 0, 0, 0);
    rightLayout->setSpacing(6);
    rightLayout->addWidget(new QLabel("<h3>PROTOCOL</h3>"));

    // Name
    auto* nameGroup = new QGroupBox("Name");
    auto* nameLayout = new QVBoxLayout(nameGroup);
    nameEdit_ = new QLineEdit;
    nameEdit_->setText(QString::fromStdString(editing_.name));
    connect(nameEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        editing_.name = text.toStdString();
    });
    nameLayout->addWidget(nameEdit_);
    rightLayout->addWidget(nameGroup);

    // Edit group
    auto* editGroup = new QGroupBox("Edit");
    auto* editLayout = new QVBoxLayout(editGroup);
    auto* addBtn = new QPushButton("Add Event");
    auto* sortBtn = new QPushButton("Sort by Time");
    auto* clearBtn = new QPushButton("Clear All");
    connect(addBtn, &QPushButton::clicked, this, &ProtocolEditorTab::onAddEvent);
    connect(sortBtn, &QPushButton::clicked, this, &ProtocolEditorTab::onSortByTime);
    connect(clearBtn, &QPushButton::clicked, this, &ProtocolEditorTab::onClearAll);
    editLayout->addWidget(addBtn);
    editLayout->addWidget(sortBtn);
    editLayout->addWidget(clearBtn);
    rightLayout->addWidget(editGroup);

    // File group
    auto* fileGroup = new QGroupBox("File");
    auto* fileLayout = new QVBoxLayout(fileGroup);
    auto* saveBtn = new QPushButton("Save Protocol");
    auto* saveAsBtn = new QPushButton("Save As…");
    auto* loadBtn = new QPushButton("Load Protocol…");
    connect(saveBtn, &QPushButton::clicked, this, &ProtocolEditorTab::onSaveProtocol);
    connect(saveAsBtn, &QPushButton::clicked, this, &ProtocolEditorTab::onSaveAs);
    connect(loadBtn, &QPushButton::clicked, this, &ProtocolEditorTab::onLoadProtocol);
    fileLayout->addWidget(saveBtn);
    fileLayout->addWidget(saveAsBtn);
    fileLayout->addWidget(loadBtn);
    rightLayout->addWidget(fileGroup);

    // Saved protocols
    auto* savedGroup = new QGroupBox("Saved Protocols");
    auto* savedLayout = new QVBoxLayout(savedGroup);
    savedList_ = new QListWidget;
    connect(savedList_, &QListWidget::itemDoubleClicked, this, &ProtocolEditorTab::onSavedProtocolSelected);
    savedLayout->addWidget(savedList_);
    auto* refreshBtn = new QPushButton("Refresh List");
    connect(refreshBtn, &QPushButton::clicked, this, &ProtocolEditorTab::onRefreshList);
    savedLayout->addWidget(refreshBtn);
    rightLayout->addWidget(savedGroup, 1);

    // Validate
    auto* validateGroup = new QGroupBox("Validate");
    auto* validateLayout = new QVBoxLayout(validateGroup);
    auto* validateBtn = new QPushButton("Validate Protocol");
    connect(validateBtn, &QPushButton::clicked, this, &ProtocolEditorTab::onValidate);
    validateLayout->addWidget(validateBtn);
    validationLabel_ = new QLabel;
    validationLabel_->setWordWrap(true);
    validateLayout->addWidget(validationLabel_);
    rightLayout->addWidget(validateGroup);

    splitter->addWidget(rightPane);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({900, 400});

    // Stay in sync when the data directories change (setup dialog) or
    // protocols are saved elsewhere
    connect(ctrl_, &AppController::configChanged, this, [this] { rebuildSavedList(); });

    rebuildEventTable();
    rebuildSavedList();
}

void ProtocolEditorTab::rebuildEventTable() {
    table_->blockSignals(true);
    table_->setHorizontalHeaderLabels(
        {relativeTime_ ? "Delay (s)" : "Time (s)", "Event", "Parameter", "Order", "Actions"});
    table_->setRowCount((int)editing_.events.size());
    timeSpins_.assign(editing_.events.size(), nullptr);

    for (int i = 0; i < (int)editing_.events.size(); i++) {
        table_->setRowHeight(i, 34);
        auto& e = editing_.events[i];

        // Time — absolute, or delay since previous event (stored time stays absolute)
        auto* timeSpin = new QDoubleSpinBox;
        timeSpin->setDecimals(1);
        timeSpin->setRange(0, 100000);
        const float prevTime = (i > 0) ? editing_.events[i - 1].timeOffsetSeconds : 0.0f;
        if (relativeTime_) {
            timeSpin->setPrefix("+");
            timeSpin->setValue(e.timeOffsetSeconds - prevTime);
            timeSpin->setToolTip(QString("Delay after previous event\nAbsolute: %1 s from start")
                                 .arg(e.timeOffsetSeconds, 0, 'f', 1));
        } else {
            timeSpin->setValue(e.timeOffsetSeconds);
            if (i > 0)
                timeSpin->setToolTip(QString("%1 s after previous event")
                                     .arg(e.timeOffsetSeconds - prevTime, 0, 'f', 1));
        }
        connect(timeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, i](double v) {
                    if (i >= (int)editing_.events.size()) return;
                    if (relativeTime_) {
                        const float prev = (i > 0) ? editing_.events[i - 1].timeOffsetSeconds : 0.0f;
                        editing_.events[i].timeOffsetSeconds = prev + (float)std::max(0.0, v);
                    } else {
                        editing_.events[i].timeOffsetSeconds = (float)v;
                    }
                    refreshTimeSpins(i);
                });
        timeSpins_[i] = timeSpin;
        table_->setCellWidget(i, 0, timeSpin);

        // Event type combo
        auto* typeCombo = new QComboBox;
        for (int t = 0; t < protocolEventTypeCount(); t++) {
            typeCombo->addItem(protocolEventTypeName((ProtocolEventType)t));
        }
        typeCombo->setCurrentIndex((int)e.type);
        connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, i](int t) {
                    if (i < (int)editing_.events.size()) {
                        const auto newType = protocolEventTypeFromIndex(t);
                        editing_.events[i].type = newType;
                        editing_.events[i].parameter = defaultParameterFor(newType);
                        rebuildEventTable();
                    }
                });
        table_->setCellWidget(i, 1, typeCombo);

        // Parameter editor — varies by type
        QWidget* paramWidget = nullptr;
        if (e.type == ProtocolEventType::Countdown) {
            auto* w = new QWidget;
            auto* lay = new QHBoxLayout(w);
            lay->setContentsMargins(0, 0, 0, 0);
            auto* secs = new QSpinBox;
            secs->setRange(1, 30);
            secs->setSuffix(" s");
            auto* go = new QDoubleSpinBox;
            go->setRange(0.5, 10.0);
            go->setSingleStep(0.5);
            go->setSuffix(" GO");
            QStringList parts = QString::fromStdString(e.parameter).split(',');
            secs->setValue(parts.value(0, "3").toInt());
            go->setValue(parts.value(1, "2.0").toDouble());
            auto update = [this, i, secs, go]() {
                if (i < (int)editing_.events.size()) {
                    editing_.events[i].parameter =
                        QString("%1,%2").arg(secs->value()).arg(go->value(), 0, 'f', 1).toStdString();
                }
            };
            connect(secs, QOverload<int>::of(&QSpinBox::valueChanged), this, update);
            connect(go, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, update);
            lay->addWidget(secs);
            lay->addWidget(go);
            paramWidget = w;
        } else if (e.type == ProtocolEventType::AddFlag) {
            auto* edit = new QLineEdit(QString::fromStdString(e.parameter));
            edit->setPlaceholderText("Flag label");
            connect(edit, &QLineEdit::textChanged, this,
                    [this, i](const QString& s) {
                        if (i < (int)editing_.events.size())
                            editing_.events[i].parameter = s.toStdString();
                    });
            paramWidget = edit;
        } else if (e.type == ProtocolEventType::DisplayText) {
            auto* w = new QWidget;
            auto* lay = new QHBoxLayout(w);
            lay->setContentsMargins(0, 0, 0, 0);
            auto* text = new QLineEdit;
            auto* pos = new QComboBox;
            pos->addItems({"top", "middle", "bottom"});
            auto* scale = new QDoubleSpinBox;
            scale->setRange(0.5, 5.0);
            scale->setSingleStep(0.1);
            auto* color = new QComboBox;
            color->addItems({"white", "red", "green", "blue", "yellow", "cyan"});
            QStringList parts = QString::fromStdString(e.parameter).split('|');
            text->setText(parts.value(0));
            pos->setCurrentText(parts.value(1, "top"));
            scale->setValue(parts.value(2, "1.0").toDouble());
            color->setCurrentText(parts.value(3, "white"));
            auto update = [this, i, text, pos, scale, color]() {
                if (i < (int)editing_.events.size()) {
                    editing_.events[i].parameter =
                        QString("%1|%2|%3|%4")
                            .arg(text->text())
                            .arg(pos->currentText())
                            .arg(scale->value(), 0, 'f', 1)
                            .arg(color->currentText())
                            .toStdString();
                }
            };
            connect(text, &QLineEdit::textChanged, this, update);
            connect(pos, &QComboBox::currentTextChanged, this, update);
            connect(scale, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, update);
            connect(color, &QComboBox::currentTextChanged, this, update);
            lay->addWidget(text, 1);
            lay->addWidget(pos);
            lay->addWidget(scale);
            lay->addWidget(color);
            paramWidget = w;
        } else if (e.type == ProtocolEventType::DisplayShape) {
            auto* combo = new QComboBox;
            combo->addItems({"circle", "square", "triangle"});
            int idx = 0;
            if (e.parameter == "square") idx = 1;
            else if (e.parameter == "triangle") idx = 2;
            combo->setCurrentIndex(idx);
            connect(combo, &QComboBox::currentTextChanged, this,
                    [this, i](const QString& s) {
                        if (i < (int)editing_.events.size())
                            editing_.events[i].parameter = s.toStdString();
                    });
            paramWidget = combo;
        } else if (e.type == ProtocolEventType::SetBiofeedbackTransform) {
            auto* w = new QWidget;
            auto* lay = new QHBoxLayout(w);
            lay->setContentsMargins(0, 0, 0, 0);
            auto* angle = new QComboBox;
            for (int ai = 0; ai < biomechAngleCount(); ++ai) angle->addItem(biomechAngleName(ai));
            auto* startScale = new QDoubleSpinBox;
            auto* endScale = new QDoubleSpinBox;
            auto* rampStart = new QDoubleSpinBox;
            auto* rampDuration = new QDoubleSpinBox;
            for (auto* spin : {startScale, endScale}) {
                spin->setRange(0.1, 3.0);
                spin->setSingleStep(0.05);
                spin->setDecimals(2);
            }
            for (auto* spin : {rampStart, rampDuration}) {
                spin->setRange(0.0, 600.0);
                spin->setSingleStep(0.5);
                spin->setDecimals(1);
            }
            QStringList parts = QString::fromStdString(e.parameter).split(',');
            int angleIdx = biomechAngleFromName(parts.value(0).toStdString());
            angle->setCurrentIndex(angleIdx >= 0 ? angleIdx : 0);
            startScale->setValue(parts.value(1, "1.0").toDouble());
            endScale->setValue(parts.value(2, "1.0").toDouble());
            rampStart->setValue(parts.value(3, "0.0").toDouble());
            rampDuration->setValue(parts.value(4, "0.0").toDouble());
            auto update = [this, i, angle, startScale, endScale, rampStart, rampDuration]() {
                if (i < (int)editing_.events.size()) {
                    editing_.events[i].parameter =
                        QString("%1,%2,%3,%4,%5")
                            .arg(angle->currentText())
                            .arg(startScale->value(), 0, 'f', 2)
                            .arg(endScale->value(), 0, 'f', 2)
                            .arg(rampStart->value(), 0, 'f', 1)
                            .arg(rampDuration->value(), 0, 'f', 1)
                            .toStdString();
                }
            };
            connect(angle, &QComboBox::currentTextChanged, this, update);
            connect(startScale, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, update);
            connect(endScale, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, update);
            connect(rampStart, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, update);
            connect(rampDuration, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, update);
            lay->addWidget(angle, 1);
            lay->addWidget(startScale);
            lay->addWidget(endScale);
            lay->addWidget(rampStart);
            lay->addWidget(rampDuration);
            paramWidget = w;
        } else if (e.type == ProtocolEventType::PlaySound) {
            auto* w = new QWidget;
            auto* lay = new QHBoxLayout(w);
            lay->setContentsMargins(0, 0, 0, 0);
            auto* soundCombo = new QComboBox;
            soundCombo->addItems({"Beep", "Chime", "Alert", "Custom WAV…"});
            auto* pathEdit = new QLineEdit;
            pathEdit->setPlaceholderText("Path to .wav file");
            auto* browseBtn = new QPushButton("…");
            browseBtn->setMaximumWidth(28);
            browseBtn->setToolTip("Browse for a WAV file");
            auto* testBtn = new QPushButton("►");
            testBtn->setMaximumWidth(28);
            testBtn->setToolTip("Preview this sound");

            // Initialise from the stored parameter
            const QString param = QString::fromStdString(e.parameter);
            int comboIdx = 3;
            if (param == "beep") comboIdx = 0;
            else if (param == "chime") comboIdx = 1;
            else if (param == "alert") comboIdx = 2;
            soundCombo->setCurrentIndex(comboIdx);
            if (comboIdx == 3) pathEdit->setText(param);
            const bool custom = (comboIdx == 3);
            pathEdit->setVisible(custom);
            browseBtn->setVisible(custom);

            auto update = [this, i, soundCombo, pathEdit, browseBtn]() {
                if (i >= (int)editing_.events.size()) return;
                static const char* builtin[] = {"beep", "chime", "alert"};
                const int idx = soundCombo->currentIndex();
                const bool custom = (idx == 3);
                pathEdit->setVisible(custom);
                browseBtn->setVisible(custom);
                editing_.events[i].parameter =
                    custom ? pathEdit->text().toStdString() : builtin[idx];
            };
            connect(soundCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, update);
            connect(pathEdit, &QLineEdit::textChanged, this, update);
            connect(browseBtn, &QPushButton::clicked, this, [this, pathEdit]() {
                QString path = QFileDialog::getOpenFileName(this, "Choose WAV File",
                    QString(), "WAV Files (*.wav)");
                if (!path.isEmpty()) pathEdit->setText(path);
            });
            connect(testBtn, &QPushButton::clicked, this, [this, i]() {
                if (i < (int)editing_.events.size())
                    ctrl_->playSound(editing_.events[i].parameter);
            });

            lay->addWidget(soundCombo);
            lay->addWidget(pathEdit, 1);
            lay->addWidget(browseBtn);
            lay->addWidget(testBtn);
            paramWidget = w;
        } else if (e.type == ProtocolEventType::ClearBiofeedbackTransform) {
            auto* combo = new QComboBox;
            combo->addItem("ALL");
            for (int ai = 0; ai < biomechAngleCount(); ++ai) combo->addItem(biomechAngleName(ai));
            int idx = combo->findText(QString::fromStdString(e.parameter));
            combo->setCurrentIndex(idx >= 0 ? idx : 0);
            connect(combo, &QComboBox::currentTextChanged, this,
                    [this, i](const QString& s) {
                        if (i < (int)editing_.events.size())
                            editing_.events[i].parameter = s.toStdString();
                    });
            paramWidget = combo;
        } else if (e.type == ProtocolEventType::StartCamera ||
                   e.type == ProtocolEventType::StopCamera ||
                   e.type == ProtocolEventType::StartRecording ||
                   e.type == ProtocolEventType::StopRecording ||
                   e.type == ProtocolEventType::ClearOverlay ||
                   e.type == ProtocolEventType::HideAvatar ||
                   e.type == ProtocolEventType::ShowAvatar ||
                   e.type == ProtocolEventType::BeginBiofeedback ||
                   e.type == ProtocolEventType::EndBiofeedback) {
            auto* lbl = new QLabel("-");
            lbl->setStyleSheet("color: #888;");
            paramWidget = lbl;
        } else {
            auto* edit = new QLineEdit(QString::fromStdString(e.parameter));
            connect(edit, &QLineEdit::textChanged, this,
                    [this, i](const QString& s) {
                        if (i < (int)editing_.events.size())
                            editing_.events[i].parameter = s.toStdString();
                    });
            paramWidget = edit;
        }
        table_->setCellWidget(i, 2, paramWidget);

        // Order (up/down)
        auto* orderWidget = new QWidget;
        auto* orderLayout = new QHBoxLayout(orderWidget);
        orderLayout->setContentsMargins(0, 0, 0, 0);
        orderLayout->setSpacing(2);
        auto* upBtn = new QPushButton("^");
        auto* downBtn = new QPushButton("v");
        upBtn->setMaximumWidth(25);
        downBtn->setMaximumWidth(25);
        upBtn->setEnabled(i > 0);
        downBtn->setEnabled(i < (int)editing_.events.size() - 1);
        connect(upBtn, &QPushButton::clicked, this, [this, i]() {
            if (i > 0) {
                std::swap(editing_.events[i], editing_.events[i - 1]);
                rebuildEventTable();
            }
        });
        connect(downBtn, &QPushButton::clicked, this, [this, i]() {
            if (i < (int)editing_.events.size() - 1) {
                std::swap(editing_.events[i], editing_.events[i + 1]);
                rebuildEventTable();
            }
        });
        orderLayout->addWidget(upBtn);
        orderLayout->addWidget(downBtn);
        table_->setCellWidget(i, 3, orderWidget);

        // Actions (duplicate + delete)
        auto* actWidget = new QWidget;
        auto* actLayout = new QHBoxLayout(actWidget);
        actLayout->setContentsMargins(0, 0, 0, 0);
        actLayout->setSpacing(2);
        auto* dupBtn = new QPushButton("D");
        auto* delBtn = new QPushButton("X");
        dupBtn->setMaximumWidth(25);
        delBtn->setMaximumWidth(25);
        dupBtn->setToolTip("Duplicate this event");
        delBtn->setToolTip("Delete this event");
        connect(dupBtn, &QPushButton::clicked, this, [this, i]() {
            if (i >= 0 && i < (int)editing_.events.size()) {
                ProtocolEvent dup = editing_.events[i];
                dup.timeOffsetSeconds += 1.0f;
                editing_.events.insert(editing_.events.begin() + i + 1, dup);
                rebuildEventTable();
            }
        });
        connect(delBtn, &QPushButton::clicked, this, [this, i]() {
            if (i >= 0 && i < (int)editing_.events.size()) {
                editing_.events.erase(editing_.events.begin() + i);
                rebuildEventTable();
            }
        });
        actLayout->addWidget(dupBtn);
        actLayout->addWidget(delBtn);
        table_->setCellWidget(i, 4, actWidget);
    }
    table_->blockSignals(false);
}

// After a time edit, update the other rows' spinboxes in place: in relative
// mode a change to one row shifts the next row's displayed delay; in absolute
// mode only tooltips change. Skips the row being edited to not fight typing.
void ProtocolEditorTab::refreshTimeSpins(int excludeRow) {
    for (int i = 0; i < (int)timeSpins_.size() && i < (int)editing_.events.size(); i++) {
        auto* spin = timeSpins_[i];
        if (!spin) continue;
        const float prevTime = (i > 0) ? editing_.events[i - 1].timeOffsetSeconds : 0.0f;
        const float absTime = editing_.events[i].timeOffsetSeconds;
        if (relativeTime_) {
            spin->setToolTip(QString("Delay after previous event\nAbsolute: %1 s from start")
                             .arg(absTime, 0, 'f', 1));
            if (i != excludeRow) {
                spin->blockSignals(true);
                spin->setValue(absTime - prevTime);
                spin->blockSignals(false);
            }
        } else if (i > 0) {
            spin->setToolTip(QString("%1 s after previous event")
                             .arg(absTime - prevTime, 0, 'f', 1));
        }
    }
}

void ProtocolEditorTab::rebuildSavedList() {
    savedList_->clear();
    auto files = listProtocolFiles(ctrl_->config().protocolsDir);
    for (auto& f : files) savedList_->addItem(QString::fromStdString(f));
}

void ProtocolEditorTab::onAddEvent() {
    ProtocolEvent e;
    e.type = ProtocolEventType::StartCamera;
    e.timeOffsetSeconds = editing_.events.empty() ? 0.0f
        : editing_.events.back().timeOffsetSeconds + 5.0f;
    editing_.events.push_back(e);
    rebuildEventTable();
}

void ProtocolEditorTab::onSortByTime() {
    editing_.sortByTime();
    rebuildEventTable();
}

void ProtocolEditorTab::onClearAll() {
    if (QMessageBox::question(this, "Clear All",
        "Remove all events from this protocol?") == QMessageBox::Yes) {
        editing_.events.clear();
        rebuildEventTable();
    }
}

void ProtocolEditorTab::onSaveProtocol() {
    if (editing_.name.empty()) {
        QMessageBox::warning(this, "Save", "Please enter a protocol name.");
        return;
    }
    editing_.sortByTime();
    std::string err = editing_.validate();
    if (!err.empty()) {
        validationLabel_->setText(QString("<span style='color:#e05050;'>Cannot save: %1</span>")
                                  .arg(QString::fromStdString(err)));
        return;
    }
    std::string filename = editing_.name;
    for (auto& c : filename) if (c == ' ') c = '_';
    std::string path = ctrl_->config().protocolsDir + "/" + filename + ".json";
    fs::create_directories(ctrl_->config().protocolsDir);
    if (editing_.saveJSON(path)) {
        rebuildSavedList();
        ctrl_->notifyProtocolsChanged();
        validationLabel_->setText("<span style='color:#50e050;'>Saved.</span>");
    } else {
        validationLabel_->setText("<span style='color:#e05050;'>Failed to save file.</span>");
    }
}

void ProtocolEditorTab::onSaveAs() {
    editing_.sortByTime();
    std::string err = editing_.validate();
    if (!err.empty()) {
        validationLabel_->setText(QString("<span style='color:#e05050;'>Cannot save: %1</span>")
                                  .arg(QString::fromStdString(err)));
        return;
    }
    QString path = QFileDialog::getSaveFileName(this, "Save Protocol As",
        QString::fromStdString(ctrl_->config().protocolsDir), "JSON Files (*.json)");
    if (path.isEmpty()) return;
    editing_.name = nameEdit_->text().toStdString();
    if (editing_.saveJSON(path.toStdString())) {
        rebuildSavedList();
        ctrl_->notifyProtocolsChanged();
        validationLabel_->setText("<span style='color:#50e050;'>Saved.</span>");
    } else {
        validationLabel_->setText("<span style='color:#e05050;'>Failed to save file.</span>");
    }
}

void ProtocolEditorTab::onLoadProtocol() {
    QString path = QFileDialog::getOpenFileName(this, "Load Protocol",
        QString::fromStdString(ctrl_->config().protocolsDir), "JSON Files (*.json)");
    if (path.isEmpty()) return;
    if (editing_.loadJSON(path.toStdString())) {
        nameEdit_->setText(QString::fromStdString(editing_.name));
        rebuildEventTable();
        validationLabel_->clear();
    } else {
        validationLabel_->setText("<span style='color:#e05050;'>Failed to load file.</span>");
    }
}

void ProtocolEditorTab::onValidate() {
    editing_.sortByTime();
    std::string err = editing_.validate();
    if (err.empty()) {
        validationLabel_->setText("<span style='color:#50e050;'>Protocol is valid!</span>");
    } else {
        validationLabel_->setText(QString("<span style='color:#e05050;'>%1</span>")
                                  .arg(QString::fromStdString(err)));
    }
    rebuildEventTable();
}

void ProtocolEditorTab::onRefreshList() { rebuildSavedList(); }

void ProtocolEditorTab::onSavedProtocolSelected() {
    auto* item = savedList_->currentItem();
    if (!item) return;
    std::string path = ctrl_->config().protocolsDir + "/" + item->text().toStdString();
    if (editing_.loadJSON(path)) {
        nameEdit_->setText(QString::fromStdString(editing_.name));
        rebuildEventTable();
    }
}
