#include "main_window.h"
#include "clinical_record_tab.h"
#include "clinical_settings_dialog.h"
#include "live_tab.h"
#include "playback_tab.h"
#include "protocol_editor_tab.h"
#include "data_tab.h"
#include "experimental_tab.h"
#include "report_tab.h"
#include "info_tab.h"
#include "../app_controller.h"

#include <QMessageBox>
#include <QToolButton>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

MainWindow::MainWindow(AppController* ctrl, UiMode mode, QWidget* parent)
    : QMainWindow(parent), ctrl_(ctrl), mode_(mode) {
    setWindowTitle(mode_ == UiMode::Clinical
        ? "York Clinical Tracking Suite"
        : "York Clinical Tracking Suite — Research UI");
    resize(1400, 800);
    setMinimumSize(1000, 600);
    setProperty("dsAppShell", true);

    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    setCentralWidget(tabs_);

    if (mode_ == UiMode::Clinical) buildClinicalTabs();
    else buildResearchTabs();

    statusBar_ = statusBar();
    statusBar_->setProperty("dsStatusBar", true);
    if (!ctrl_->sdkWarning().empty()) {
        statusBar_->showMessage(QString::fromStdString(ctrl_->sdkWarning()));
    } else {
        statusBar_->showMessage("Ready");
    }

    connect(ctrl_, &AppController::errorOccurred, this, &MainWindow::onError);
}

// A simple monochrome gear so the settings button matches the theme on
// every platform (emoji glyphs render as coloured bitmaps on Windows).
QIcon MainWindow::makeGearIcon(const QColor& color) {
    const int size = 64;
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF c(size / 2.0, size / 2.0);
    const double outer = 28, inner = 21, hole = 9;
    QPainterPath path;
    const int teeth = 8;
    for (int i = 0; i < teeth; ++i) {
        const double a0 = (2 * M_PI * i) / teeth;
        const double step = M_PI / teeth;
        auto pt = [&](double r, double a) { return QPointF(c.x() + r * qCos(a), c.y() + r * qSin(a)); };
        if (i == 0) path.moveTo(pt(outer, a0 - step * 0.35));
        path.lineTo(pt(outer, a0 + step * 0.35));
        path.lineTo(pt(inner, a0 + step * 0.65));
        path.lineTo(pt(inner, a0 + step * 1.35));
        path.lineTo(pt(outer, a0 + step * 1.65));
    }
    path.closeSubpath();
    path.addEllipse(c, hole, hole);
    path.setFillRule(Qt::OddEvenFill);
    p.fillPath(path, color);
    return QIcon(pm);
}

// Clinical UI layer: only what a clinic session needs — record a patient,
// review a recording. Everything else lives in the research UI.
void MainWindow::buildClinicalTabs() {
    recordTab_ = new ClinicalRecordTab(ctrl_);
    // The Viewer is the upstream playback tab; without an Experimental tab the
    // legacy/resampled loaders simply stay hidden.
    playbackTab_ = new PlaybackTab(ctrl_, nullptr);

    tabs_->addTab(recordTab_, "Record");
    tabs_->addTab(playbackTab_, "Viewer");

    // Gear button in the tab bar's top-right corner: clinic defaults
    // (camera, device, protocol, clinician) that the Record tab pre-selects.
    auto* settingsBtn = new QToolButton;
    settingsBtn->setIcon(makeGearIcon(QColor("#5b6b7b")));
    settingsBtn->setIconSize(QSize(18, 18));
    settingsBtn->setToolTip("Defaults: camera, protocol and clinician pre-selected at start-up");
    settingsBtn->setAutoRaise(true);
    settingsBtn->setCursor(Qt::PointingHandCursor);
    settingsBtn->setProperty("settingsButton", true);
    connect(settingsBtn, &QToolButton::clicked, this, &MainWindow::onOpenSettings);
    tabs_->setCornerWidget(settingsBtn, Qt::TopRightCorner);
}

// Research UI: the full upstream tab set.
void MainWindow::buildResearchTabs() {
    liveTab_ = new LiveTab(ctrl_);
    protocolEditorTab_ = new ProtocolEditorTab(ctrl_);
    dataTab_ = new DataTab(ctrl_);
    experimentalTab_ = new ExperimentalTab(ctrl_);
    infoTab_ = new InfoTab;

    // Playback tab needs access to ExperimentalTab for legacy/resampled toggles
    playbackTab_ = new PlaybackTab(ctrl_, experimentalTab_);

    tabs_->addTab(liveTab_, "Live");
    tabs_->addTab(playbackTab_, "Playback");
    tabs_->addTab(protocolEditorTab_, "Protocol Editor");
    tabs_->addTab(dataTab_, "Data");
    tabs_->addTab(experimentalTab_, "Experimental");
    tabs_->addTab(infoTab_, "Info");

    // Experimental: the Report tab is added/removed via the Experimental tab
    connect(experimentalTab_, &ExperimentalTab::reportTabToggled, this,
            [this](bool enabled) {
        if (enabled && !reportTab_) {
            reportTab_ = new ReportTab(ctrl_);
            // insert just before the Experimental tab
            const int idx = tabs_->indexOf(experimentalTab_);
            tabs_->insertTab(idx, reportTab_, "Report");
            tabs_->setCurrentWidget(reportTab_);
        } else if (!enabled && reportTab_) {
            const int idx = tabs_->indexOf(reportTab_);
            if (idx >= 0) tabs_->removeTab(idx);
            delete reportTab_;
            reportTab_ = nullptr;
        }
    });
}

MainWindow::~MainWindow() = default;

void MainWindow::onError(QString message) {
    QMessageBox::warning(this, "Error", message);
}

void MainWindow::onOpenSettings() {
    ClinicalSettingsDialog dialog(ctrl_, this);
    if (dialog.exec() == QDialog::Accepted && recordTab_)
        recordTab_->applyDefaults();
}
