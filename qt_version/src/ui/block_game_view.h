#pragma once

#include <QWidget>

class BlockGame;

class BlockGameView : public QWidget {
    Q_OBJECT
public:
    explicit BlockGameView(QWidget* parent = nullptr);

    void setGame(BlockGame* game) { game_ = game; update(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    BlockGame* game_ = nullptr;
};
