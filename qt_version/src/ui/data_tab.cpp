#include "data_tab.h"
#include "../app_controller.h"
#include "../data/session_scanner.h"
#include "../data/data_exporter.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QLineEdit>
#include <QComboBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QProgressBar>
#include <QScrollArea>
#include <QCheckBox>
#include <algorithm>

DataTab::DataTab(AppController* ctrl, QWidget* parent)
    : QWidget(parent), ctrl_(ctrl) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // Top bar: filter + buttons
    auto* topBar = new QHBoxLayout;
    topBar->setSpacing(6);
    filterEdit_ = new QLineEdit;
    filterEdit_->setPlaceholderText("Filter by patient ID, protocol, session name, notes…");
    filterEdit_->setMaximumWidth(400);
    connect(filterEdit_, &QLineEdit::textChanged, this, &DataTab::onFilterChanged);

    protocolFilter_ = new QComboBox;
    protocolFilter_->setMaximumWidth(200);
    protocolFilter_->addItem("All Protocols", "");
    connect(protocolFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DataTab::onFilterChanged);

    selectAllBtn_ = new QPushButton("Select All");
    deselectBtn_ = new QPushButton("Deselect");
    deleteBtn_ = new QPushButton("Delete");
    deleteBtn_->setStyleSheet("QPushButton { background-color: #701515; }");
    refreshBtn_ = new QPushButton("Refresh");

    connect(selectAllBtn_, &QPushButton::clicked, this, &DataTab::onSelectAll);
    connect(deselectBtn_, &QPushButton::clicked, this, &DataTab::onDeselectAll);
    connect(deleteBtn_, &QPushButton::clicked, this, &DataTab::onDelete);
    connect(refreshBtn_, &QPushButton::clicked, this, &DataTab::refresh);

    topBar->addWidget(filterEdit_);
    topBar->addWidget(protocolFilter_);
    topBar->addWidget(selectAllBtn_);
    topBar->addWidget(deselectBtn_);
    topBar->addWidget(deleteBtn_);
    topBar->addWidget(refreshBtn_);
    topBar->addStretch();
    root->addLayout(topBar);

    // Splitter: table | detail
    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setHandleWidth(5);
    root->addWidget(splitter, 1);

    // Left: session table
    table_ = new QTableWidget;
    table_->setColumnCount(9);
    table_->setHorizontalHeaderLabels({"", "Patient ID", "Date", "Duration",
                                       "Frames", "Camera", "Protocol", "Flags", "Size"});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(28);
    table_->verticalHeader()->setMinimumSectionSize(26);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setSortingEnabled(true);

    table_->setColumnWidth(0, 30);   // checkbox
    table_->setColumnWidth(1, 120);  // patient
    table_->setColumnWidth(2, 140);  // date
    table_->setColumnWidth(3, 70);   // duration
    table_->setColumnWidth(4, 70);   // frames
    table_->setColumnWidth(5, 100);  // camera
    table_->setColumnWidth(6, 220);  // protocol
    table_->setColumnWidth(7, 50);   // flags
    table_->setColumnWidth(8, 70);   // size

    connect(table_, &QTableWidget::itemSelectionChanged,
            this, &DataTab::onSelectionChanged);
    splitter->addWidget(table_);

    // Right: detail panel (scrollable)
    auto* detailScroll = new QScrollArea;
    detailScroll->setWidgetResizable(true);
    auto* detailContent = new QWidget;
    detailScroll->setWidget(detailContent);
    auto* detailLayout = new QVBoxLayout(detailContent);
    detailLayout->setContentsMargins(8, 8, 8, 8);
    detailLayout->setSpacing(7);

    auto* header = new QLabel("<h3>SESSION DETAILS</h3>");
    detailLayout->addWidget(header);

    // Rename group
    auto* renameGroup = new QGroupBox("Rename");
    auto* renameLayout = new QHBoxLayout(renameGroup);
    renameEdit_ = new QLineEdit;
    renameBtn_ = new QPushButton("Rename");
    connect(renameBtn_, &QPushButton::clicked, this, &DataTab::onRename);
    renameLayout->addWidget(renameEdit_);
    renameLayout->addWidget(renameBtn_);
    detailLayout->addWidget(renameGroup);

    // Metadata
    auto* metaGroup = new QGroupBox("Metadata");
    auto* metaLayout = new QVBoxLayout(metaGroup);
    metaInfoLabel_ = new QLabel;
    metaInfoLabel_->setWordWrap(true);
    metaLayout->addWidget(metaInfoLabel_);

    auto* patientRow = new QHBoxLayout;
    patientRow->addWidget(new QLabel("Patient ID:"));
    patientIdEdit_ = new QLineEdit;
    patientRow->addWidget(patientIdEdit_);
    metaLayout->addLayout(patientRow);

    auto* opRow = new QHBoxLayout;
    opRow->addWidget(new QLabel("Operator:"));
    operatorEdit_ = new QLineEdit;
    opRow->addWidget(operatorEdit_);
    metaLayout->addLayout(opRow);

    auto* notesRow = new QHBoxLayout;
    notesRow->addWidget(new QLabel("Notes:"));
    notesEdit_ = new QLineEdit;
    notesRow->addWidget(notesEdit_);
    metaLayout->addLayout(notesRow);

    saveMetaBtn_ = new QPushButton("Save Metadata");
    connect(saveMetaBtn_, &QPushButton::clicked, this, &DataTab::onSaveMetadata);
    metaLayout->addWidget(saveMetaBtn_);
    detailLayout->addWidget(metaGroup);

    // Statistics
    auto* statsGroup = new QGroupBox("Statistics");
    auto* statsLayout = new QVBoxLayout(statsGroup);
    statsLabel_ = new QLabel;
    statsLabel_->setWordWrap(true);
    statsLayout->addWidget(statsLabel_);

    computeStatsBtn_ = new QPushButton("Compute Detailed Stats");
    connect(computeStatsBtn_, &QPushButton::clicked, this, &DataTab::onComputeStats);
    statsLayout->addWidget(computeStatsBtn_);

    statsProgress_ = new QProgressBar;
    statsProgress_->setVisible(false);
    statsLayout->addWidget(statsProgress_);

    anglesTable_ = new QTableWidget;
    anglesTable_->setColumnCount(4);
    anglesTable_->setHorizontalHeaderLabels({"Angle", "Min", "Max", "Mean"});
    anglesTable_->horizontalHeader()->setStretchLastSection(true);
    anglesTable_->verticalHeader()->setVisible(false);
    anglesTable_->setVisible(false);
    anglesTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    anglesTable_->setMaximumHeight(200);
    statsLayout->addWidget(anglesTable_);

    detailLayout->addWidget(statsGroup);

    statusLabel_ = new QLabel;
    statusLabel_->setWordWrap(true);
    detailLayout->addWidget(statusLabel_);

    // Export section
    auto* exportGroup = new QGroupBox("Export");
    auto* exportLayout = new QVBoxLayout(exportGroup);

    auto* fmtRow = new QHBoxLayout;
    fmtRow->addWidget(new QLabel("Format:"));
    formatCombo_ = new QComboBox;
    formatCombo_->addItem("Raw CSV");
    formatCombo_->addItem("Standardised JSON");
    fmtRow->addWidget(formatCombo_);
    exportLayout->addLayout(fmtRow);

    auto* batchRow = new QHBoxLayout;
    batchRow->addWidget(new QLabel("Batch:"));
    batchModeCombo_ = new QComboBox;
    batchModeCombo_->addItem("Individual Files");
    batchModeCombo_->addItem("Group by Patient");
    batchModeCombo_->addItem("Group by Protocol");
    batchRow->addWidget(batchModeCombo_);
    exportLayout->addLayout(batchRow);

    exportBtn_ = new QPushButton("Export…");
    connect(exportBtn_, &QPushButton::clicked, this, &DataTab::onExport);
    exportLayout->addWidget(exportBtn_);

    exportProgress_ = new QProgressBar;
    exportProgress_->setVisible(false);
    exportLayout->addWidget(exportProgress_);

    exportStatus_ = new QLabel;
    exportStatus_->setWordWrap(true);
    exportLayout->addWidget(exportStatus_);

    detailLayout->addWidget(exportGroup);

    detailLayout->addStretch();
    splitter->addWidget(detailScroll);

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({900, 400});

    // Timers
    statsTimer_ = new QTimer(this);
    statsTimer_->setInterval(100);
    connect(statsTimer_, &QTimer::timeout, this, &DataTab::onStatsTick);

    exportTimer_ = new QTimer(this);
    exportTimer_->setInterval(100);
    connect(exportTimer_, &QTimer::timeout, this, [this]() {
        auto& exp = ctrl_->dataExporter();
        if (exp.isExporting()) {
            exportProgress_->setValue((int)(exp.progress() * 100));
            exportStatus_->setText(QString::fromStdString(exp.statusMessage()));
        } else {
            exportProgress_->setVisible(false);
            exportStatus_->setText(QString::fromStdString(exp.statusMessage()));
            if (exp.hasError()) {
                exportStatus_->setStyleSheet("color: #e05050;");
            } else {
                exportStatus_->setStyleSheet("color: #50e050;");
            }
            exportTimer_->stop();
        }
    });

    // Delete-confirmation debounce
    auto* deleteResetTimer = new QTimer(this);
    deleteResetTimer->setInterval(100);
    connect(deleteResetTimer, &QTimer::timeout, this, [this]() {
        if (deleteConfirmStep_ == 1) {
            deleteConfirmTimer_ -= 0.1f;
            if (deleteConfirmTimer_ <= 0) {
                deleteConfirmStep_ = 0;
                deleteBtn_->setText("Delete");
                deleteBtn_->setStyleSheet("QPushButton { background-color: #701515; }");
            } else {
                deleteBtn_->setText(QString("Confirm Delete? (%1s)").arg((int)deleteConfirmTimer_ + 1));
            }
        }
    });
    deleteResetTimer->start();

    // Initial scan
    QTimer::singleShot(0, this, &DataTab::refresh);
}

void DataTab::refresh() {
    auto& scanner = ctrl_->sessionScanner();
    scanner.scan(ctrl_->config().recordingsDir);
    rebuildProtocolFilter();
    rebuildTable();
}

void DataTab::rebuildProtocolFilter() {
    auto& scanner = ctrl_->sessionScanner();
    QString current = protocolFilter_->currentData().toString();
    protocolFilter_->blockSignals(true);
    protocolFilter_->clear();
    protocolFilter_->addItem("All Protocols", "");
    QStringList protocols;
    for (auto& s : scanner.sessions()) {
        QString p = QString::fromStdString(s.protocol);
        if (!p.isEmpty() && !protocols.contains(p)) protocols.append(p);
    }
    protocols.sort();
    for (auto& p : protocols) protocolFilter_->addItem(p, p);
    int idx = protocolFilter_->findData(current);
    protocolFilter_->setCurrentIndex(idx >= 0 ? idx : 0);
    protocolFilter_->blockSignals(false);
}

void DataTab::rebuildTable() {
    auto& scanner = ctrl_->sessionScanner();
    QString filterText = filterEdit_->text().trimmed().toLower();
    QString protocolFilter = protocolFilter_->currentData().toString();

    table_->setSortingEnabled(false);
    table_->setRowCount(0);

    for (int i = 0; i < (int)scanner.sessions().size(); i++) {
        auto& s = scanner.sessions()[i];

        // Apply filters
        if (!filterText.isEmpty()) {
            QString hay = (QString::fromStdString(s.patientId) + " " +
                          QString::fromStdString(s.protocol) + " " +
                          QString::fromStdString(s.displayName) + " " +
                          QString::fromStdString(s.notes)).toLower();
            if (!hay.contains(filterText)) continue;
        }
        if (!protocolFilter.isEmpty() && QString::fromStdString(s.protocol) != protocolFilter) continue;

        int row = table_->rowCount();
        table_->insertRow(row);

        // Checkbox
        auto* checkItem = new QTableWidgetItem;
        checkItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        checkItem->setCheckState(s.selected ? Qt::Checked : Qt::Unchecked);
        checkItem->setData(Qt::UserRole, i);
        table_->setItem(row, 0, checkItem);

        auto* patientItem = new QTableWidgetItem(s.patientId.empty()
            ? QString::fromStdString(s.displayName)
            : QString::fromStdString(s.patientId));
        patientItem->setData(Qt::UserRole, i);
        table_->setItem(row, 1, patientItem);

        table_->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(s.date)));

        int dm = (int)s.durationSeconds / 60;
        int ds = (int)s.durationSeconds % 60;
        table_->setItem(row, 3, new QTableWidgetItem(QString("%1:%2")
            .arg(dm).arg(ds, 2, 10, QChar('0'))));

        table_->setItem(row, 4, new QTableWidgetItem(QString::number(s.frameCount)));

        QString camText = s.cameraNames.empty()
            ? QString("%1 cam").arg(s.cameraCount)
            : QString::fromStdString(s.cameraNames[0]);
        table_->setItem(row, 5, new QTableWidgetItem(camText));

        table_->setItem(row, 6, new QTableWidgetItem(QString::fromStdString(s.protocol)));

        auto* flagItem = new QTableWidgetItem(QString::number(s.flagCount));
        if (s.flagCount > 0) flagItem->setForeground(QColor(80, 160, 255));
        else flagItem->setForeground(QColor(120, 120, 120));
        table_->setItem(row, 7, flagItem);

        QString sizeText = s.totalFileSize > 1024 * 1024
            ? QString::number(s.totalFileSize / (1024.0 * 1024.0), 'f', 1) + "M"
            : QString::number(s.totalFileSize / 1024.0, 'f', 0) + "K";
        table_->setItem(row, 8, new QTableWidgetItem(sizeText));
    }

    table_->setSortingEnabled(true);

    // Listen for checkbox changes
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (item->column() != 0) return;
        int idx = item->data(Qt::UserRole).toInt();
        auto& sessions = ctrl_->sessionScanner().sessions();
        if (idx >= 0 && idx < (int)sessions.size()) {
            sessions[idx].selected = (item->checkState() == Qt::Checked);
        }
    });
}

void DataTab::onFilterChanged() { rebuildTable(); }

int DataTab::currentDetailRow() const {
    auto items = table_->selectedItems();
    if (items.isEmpty()) return -1;
    return items.first()->data(Qt::UserRole).toInt();
}

void DataTab::onSelectionChanged() {
    int idx = currentDetailRow();
    detailIdx_ = idx;
    updateDetailPanel();
}

void DataTab::updateDetailPanel() {
    auto& sessions = ctrl_->sessionScanner().sessions();
    if (detailIdx_ < 0 || detailIdx_ >= (int)sessions.size()) {
        metaInfoLabel_->clear();
        statsLabel_->clear();
        anglesTable_->setVisible(false);
        renameEdit_->clear();
        patientIdEdit_->clear();
        operatorEdit_->clear();
        notesEdit_->clear();
        return;
    }
    auto& s = sessions[detailIdx_];
    renameEdit_->setText(QString::fromStdString(s.displayName));
    patientIdEdit_->setText(QString::fromStdString(s.patientId));
    operatorEdit_->setText(QString::fromStdString(s.operatorName));
    notesEdit_->setText(QString::fromStdString(s.notes));

    metaInfoLabel_->setText(QString(
        "<b>Name:</b> %1<br/>"
        "<b>Date:</b> %2<br/>"
        "<b>Protocol:</b> %3<br/>"
        "<b>Biofeedback:</b> %4")
        .arg(QString::fromStdString(s.displayName))
        .arg(QString::fromStdString(s.date))
        .arg(QString::fromStdString(s.protocol))
        .arg(s.biofeedbackActive ? "Active" : "Inactive"));

    QString statsText = QString(
        "<b>Duration:</b> %1 s<br/>"
        "<b>Frames:</b> %2<br/>"
        "<b>Cameras:</b> %3<br/>"
        "<b>Flags:</b> %4<br/>"
        "<b>Size:</b> %5 MB")
        .arg(s.durationSeconds, 0, 'f', 1)
        .arg(s.frameCount)
        .arg(s.cameraCount)
        .arg(s.flagCount)
        .arg(s.totalFileSize / (1024.0 * 1024.0), 0, 'f', 1);

    if (s.detailedStatsLoaded) {
        statsText += QString(
            "<br/><b>Avg Confidence:</b> %1%<br/>"
            "<b>Tracked Frames:</b> %2%")
            .arg(s.avgConfidence * 100.0f, 0, 'f', 1)
            .arg(s.trackedFramePercent, 0, 'f', 1);

        anglesTable_->setRowCount((int)s.angleSummaries.size());
        for (int i = 0; i < (int)s.angleSummaries.size(); i++) {
            auto& a = s.angleSummaries[i];
            anglesTable_->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(a.name)));
            anglesTable_->setItem(i, 1, new QTableWidgetItem(QString::number(a.minDeg, 'f', 1)));
            anglesTable_->setItem(i, 2, new QTableWidgetItem(QString::number(a.maxDeg, 'f', 1)));
            anglesTable_->setItem(i, 3, new QTableWidgetItem(QString::number(a.meanDeg, 'f', 1)));
        }
        anglesTable_->setVisible(true);
    } else {
        anglesTable_->setVisible(false);
    }
    statsLabel_->setText(statsText);
}

void DataTab::onSelectAll() {
    ctrl_->sessionScanner().selectAll();
    rebuildTable();
}

void DataTab::onDeselectAll() {
    ctrl_->sessionScanner().deselectAll();
    rebuildTable();
    deleteConfirmStep_ = 0;
    deleteBtn_->setText("Delete");
}

void DataTab::onDelete() {
    auto selected = ctrl_->sessionScanner().selectedIndices();
    if (selected.empty()) return;

    if (deleteConfirmStep_ == 0) {
        deleteConfirmStep_ = 1;
        deleteConfirmTimer_ = 5.0f;
        deleteBtn_->setText(QString("Confirm Delete? (5s)"));
        deleteBtn_->setStyleSheet("QPushButton { background-color: #a02020; font-weight: bold; }");
    } else if (deleteConfirmStep_ == 1) {
        std::string err = ctrl_->sessionScanner().recycleSessions(selected, ctrl_->config().recordingsDir);
        if (!err.empty()) {
            QMessageBox::warning(this, "Delete Failed", QString::fromStdString(err));
        } else {
            statusLabel_->setText(QString("<span style='color:#50e050;'>%1 session(s) moved to .recycled/</span>")
                                  .arg((int)selected.size()));
            detailIdx_ = -1;
        }
        deleteConfirmStep_ = 0;
        deleteBtn_->setText("Delete");
        deleteBtn_->setStyleSheet("QPushButton { background-color: #701515; }");
        rebuildTable();
        updateDetailPanel();
    }
}

void DataTab::onRename() {
    if (detailIdx_ < 0) return;
    QString newName = renameEdit_->text().trimmed();
    if (newName.isEmpty()) return;
    std::string err = ctrl_->sessionScanner().renameSession(
        detailIdx_, newName.toStdString(), ctrl_->config().recordingsDir);
    if (err.empty()) {
        statusLabel_->setText("<span style='color:#50e050;'>Renamed successfully.</span>");
        detailIdx_ = -1;
        rebuildTable();
        updateDetailPanel();
    } else {
        statusLabel_->setText(QString("<span style='color:#e05050;'>Error: %1</span>")
                              .arg(QString::fromStdString(err)));
    }
}

void DataTab::onSaveMetadata() {
    if (detailIdx_ < 0) return;
    std::string err = ctrl_->sessionScanner().updateMetadata(
        detailIdx_,
        patientIdEdit_->text().toStdString(),
        operatorEdit_->text().toStdString(),
        notesEdit_->text().toStdString());
    if (err.empty()) {
        statusLabel_->setText("<span style='color:#50e050;'>Metadata saved.</span>");
        rebuildTable();
        updateDetailPanel();
    } else {
        statusLabel_->setText(QString("<span style='color:#e05050;'>Error: %1</span>")
                              .arg(QString::fromStdString(err)));
    }
}

void DataTab::onComputeStats() {
    if (detailIdx_ < 0) return;
    ctrl_->sessionScanner().computeDetailedStats(detailIdx_);
    statsProgress_->setVisible(true);
    statsProgress_->setRange(0, 100);
    statsTimer_->start();
}

void DataTab::onStatsTick() {
    auto& scanner = ctrl_->sessionScanner();
    if (scanner.isComputingStats()) {
        statsProgress_->setValue((int)(scanner.computeProgress() * 100));
    } else {
        statsTimer_->stop();
        statsProgress_->setVisible(false);
        updateDetailPanel();
    }
}

void DataTab::onExport() {
    auto selected = ctrl_->sessionScanner().selectedIndices();
    std::vector<SessionInfo> toExport;
    if (!selected.empty()) {
        for (int i : selected) toExport.push_back(ctrl_->sessionScanner().sessions()[i]);
    } else if (detailIdx_ >= 0) {
        toExport.push_back(ctrl_->sessionScanner().sessions()[detailIdx_]);
    } else {
        QMessageBox::information(this, "Export", "No sessions selected.");
        return;
    }

    QString dir = QFileDialog::getExistingDirectory(this, "Select Export Folder", "");
    if (dir.isEmpty()) return;

    ExportOptions opts;
    opts.format = (formatCombo_->currentIndex() == 0)
        ? ExportFormat::RawCSV : ExportFormat::StandardisedJSON;
    opts.outputDir = dir.toStdString();
    switch (batchModeCombo_->currentIndex()) {
        case 0: opts.batchMode = BatchMode::Individual; break;
        case 1: opts.batchMode = BatchMode::GroupByPatient; break;
        case 2: opts.batchMode = BatchMode::GroupByProtocol; break;
    }

    ctrl_->dataExporter().exportSessions(toExport, opts);
    exportProgress_->setVisible(true);
    exportProgress_->setValue(0);
    exportStatus_->setStyleSheet("color: #ccc;");
    exportStatus_->setText("Exporting…");
    exportTimer_->start();
}
