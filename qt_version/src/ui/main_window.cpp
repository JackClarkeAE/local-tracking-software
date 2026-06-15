#include "main_window.h"
#include "live_tab.h"
#include "playback_tab.h"
#include "protocol_editor_tab.h"
#include "data_tab.h"
#include "experimental_tab.h"
#include "info_tab.h"
#include "../app_controller.h"

#include <QMessageBox>

MainWindow::MainWindow(AppController* ctrl, QWidget* parent)
    : QMainWindow(parent), ctrl_(ctrl) {
    setWindowTitle("Local Tracking Software (Qt)");
    resize(1400, 800);
    setMinimumSize(1000, 600);
    setProperty("dsAppShell", true);

    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    setCentralWidget(tabs_);

    liveTab_ = new LiveTab(ctrl_);
    playbackTab_ = nullptr;  // created after experimentalTab_
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

    statusBar_ = statusBar();
    statusBar_->setProperty("dsStatusBar", true);
    if (!ctrl_->sdkWarning().empty()) {
        statusBar_->showMessage(QString::fromStdString(ctrl_->sdkWarning()));
    } else {
        statusBar_->showMessage("Ready");
    }

    connect(ctrl_, &AppController::errorOccurred, this, &MainWindow::onError);
}

MainWindow::~MainWindow() = default;

void MainWindow::onError(QString message) {
    QMessageBox::warning(this, "Error", message);
}
