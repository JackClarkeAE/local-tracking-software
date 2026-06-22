#pragma once
#include <QMainWindow>
#include <QTabWidget>
#include <QStatusBar>

class AppController;
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
    MainWindow(AppController* ctrl, QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onError(QString message);

private:
    AppController* ctrl_;
    QTabWidget* tabs_;
    QStatusBar* statusBar_;

    LiveTab* liveTab_;
    PlaybackTab* playbackTab_;
    ProtocolEditorTab* protocolEditorTab_;
    DataTab* dataTab_;
    ExperimentalTab* experimentalTab_;
    ReportTab* reportTab_ = nullptr;
    InfoTab* infoTab_;
};
