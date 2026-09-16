#pragma once
#include "app_ui_mode.h"
#include <QMainWindow>
#include <QTabWidget>
#include <QStatusBar>
#include <QIcon>
#include <QColor>

class AppController;
class ClinicalRecordTab;
class LiveTab;
class PlaybackTab;
class ProtocolEditorTab;
class DataTab;
class ExperimentalTab;
class ReportTab;
class InfoTab;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(AppController* ctrl, UiMode mode, QWidget* parent = nullptr);
    ~MainWindow() override;

    UiMode uiMode() const { return mode_; }

private slots:
    void onError(QString message);
    void onOpenSettings();

private:
    void buildClinicalTabs();
    void buildResearchTabs();
    static QIcon makeGearIcon(const QColor& color);

    AppController* ctrl_;
    UiMode mode_;
    QTabWidget* tabs_;
    QStatusBar* statusBar_;

    // Clinical UI layer
    ClinicalRecordTab* recordTab_ = nullptr;

    // Research UI (full upstream tab set)
    LiveTab* liveTab_ = nullptr;
    PlaybackTab* playbackTab_ = nullptr;
    ProtocolEditorTab* protocolEditorTab_ = nullptr;
    DataTab* dataTab_ = nullptr;
    ExperimentalTab* experimentalTab_ = nullptr;
    ReportTab* reportTab_ = nullptr;
    InfoTab* infoTab_ = nullptr;
};
