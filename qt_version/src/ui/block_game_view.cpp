#include "block_game_view.h"
#include "../game/block_game.h"

#include <QPainter>
#include <algorithm>

static QPointF mapGamePoint(const QRectF& r, float x, float y) {
    constexpr float arenaW = 1.75f;
    constexpr float arenaH = 1.4f;
    const double sx = r.center().x() + (x / arenaW) * (r.width() * 0.42);
    const double sy = r.center().y() - (y / arenaH) * (r.height() * 0.42);
    return QPointF(sx, sy);
}

BlockGameView::BlockGameView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(480, 270);
}

void BlockGameView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(7, 9, 18));

    QRectF arena = rect().adjusted(16, 16, -16, -16);
    p.setPen(QPen(QColor(35, 90, 120, 90), 1));
    for (int i = 0; i <= 12; ++i) {
        double t = i / 12.0;
        double y = arena.top() + t * arena.height();
        p.drawLine(QPointF(arena.left(), y), QPointF(arena.right(), y));
    }
    for (int i = 0; i <= 10; ++i) {
        double t = i / 10.0;
        double x = arena.left() + t * arena.width();
        p.drawLine(QPointF(x, arena.top()), QPointF(x, arena.bottom()));
    }

    if (!game_) {
        p.setPen(QColor(155, 161, 166));
        p.drawText(arena, Qt::AlignCenter, "Enable Block Dodge to start");
        return;
    }

    for (const auto& block : game_->blocks()) {
        double depth = std::clamp((block.z + 8.0f) / 8.0f, 0.0f, 1.0f);
        QPointF c = mapGamePoint(arena, block.x, block.y);
        double size = (0.45 + depth * 1.2) * 42.0;
        QColor color((int)(block.r * 255), (int)(block.g * 255), (int)(block.b * 255), 210);
        QRectF br(c.x() - size * 0.5, c.y() - size * 0.5, size, size);
        p.setPen(QPen(color.lighter(140), 2));
        p.setBrush(QColor(color.red(), color.green(), color.blue(), 95));
        p.drawRoundedRect(br, 4, 4);
    }

    auto drawSaber = [&](const SaberState& saber, const QColor& color) {
        if (!saber.tracked && saber.posX == 0.0f && saber.posY == 0.0f) return;
        QPointF hand = mapGamePoint(arena, saber.posX, saber.posY);
        QPointF tip = mapGamePoint(arena, saber.tipX, saber.tipY);
        p.setPen(QPen(color, 7, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(hand, tip);
        p.setPen(QPen(color.lighter(160), 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(hand, tip);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(hand, 5, 5);
        p.drawEllipse(tip, 7, 7);
    };

    drawSaber(game_->leftSaber(), QColor(0, 220, 255));
    drawSaber(game_->rightSaber(), QColor(255, 55, 65));

    if (game_->state() == GameState::Idle) {
        p.setPen(QColor(155, 161, 166));
        p.drawText(arena, Qt::AlignCenter, "Idle");
    } else if (game_->state() == GameState::GameOver) {
        p.setPen(QColor(255, 85, 95));
        p.setFont(QFont("Segoe UI", 22, QFont::Bold));
        p.drawText(arena, Qt::AlignCenter, "Game Over");
    }
}
