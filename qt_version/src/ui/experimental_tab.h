#pragma once
#include "../game/block_game.h"
#include <QWidget>
#include <QElapsedTimer>

class AppController;
class BlockGameView;
class QCheckBox;
class QLabel;
class QPushButton;
class QTimer;

class ExperimentalTab : public QWidget {
    Q_OBJECT
public:
    explicit ExperimentalTab(AppController* ctrl, QWidget* parent = nullptr);
    ~ExperimentalTab() override;

    bool binaryExportEnabled() const;
    bool legacyPlaybackEnabled() const;
    bool resampledPlaybackEnabled() const;

signals:
    void legacyPlaybackToggled(bool enabled);
    void resampledPlaybackToggled(bool enabled);
    void reportTabToggled(bool enabled);

private slots:
    void onBinaryExportToggled(bool checked);
    void writeBinaryFrame();
    void onStartGame();
    void onStopGame();
    void onResetGame();
    void updateGameFrame();

private:
    AppController* ctrl_;
    QCheckBox* binaryExportCb_ = nullptr;
    QCheckBox* legacyPlaybackCb_ = nullptr;
    QCheckBox* resampledPlaybackCb_ = nullptr;
    QCheckBox* reportTabCb_ = nullptr;
    QLabel* binaryStatusLabel_ = nullptr;
    QLabel* binaryPathLabel_ = nullptr;
    QTimer* writeTimer_ = nullptr;

    QCheckBox* blockGameCb_ = nullptr;
    QPushButton* startGameBtn_ = nullptr;
    QPushButton* stopGameBtn_ = nullptr;
    QPushButton* resetGameBtn_ = nullptr;
    QLabel* gameStatusLabel_ = nullptr;
    BlockGameView* gameView_ = nullptr;
    QTimer* gameTimer_ = nullptr;
    QElapsedTimer gameClock_;
    qint64 lastGameMs_ = 0;
    BlockGame blockGame_;

    FILE* angleBinFile_ = nullptr;
    std::string angleBinPath_;
};
