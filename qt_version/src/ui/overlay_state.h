#pragma once

#include <QColor>
#include <string>

enum class OverlayShape {
    None,
    RedCircle,
    BlueSquare,
    GreenTriangle
};

enum class TextPosition {
    Top,
    Middle,
    Bottom
};

struct OverlayState {
    std::string text;
    TextPosition textPosition = TextPosition::Top;
    float textScale = 1.0f;
    QColor textColor = QColor(255, 255, 255);
    OverlayShape shape = OverlayShape::None;
};
