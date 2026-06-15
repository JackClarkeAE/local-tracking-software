#include "design_system.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QIODevice>
#include <QPalette>
#include <QStyleFactory>

namespace {

QColor slate(int r, int g, int b) {
    return QColor(r, g, b);
}

void setDisabled(QPalette& palette, QPalette::ColorRole role, const QColor& color) {
    palette.setColor(QPalette::Disabled, role, color);
}

}

namespace DesignSystem {

void apply(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));

    QFont font("Segoe UI", 9);
    font.setStyleHint(QFont::SansSerif);
    app.setFont(font);

    QPalette palette;
    palette.setColor(QPalette::Window, slate(16, 20, 26));
    palette.setColor(QPalette::WindowText, slate(236, 241, 247));
    palette.setColor(QPalette::Base, slate(18, 24, 32));
    palette.setColor(QPalette::AlternateBase, slate(24, 31, 41));
    palette.setColor(QPalette::ToolTipBase, slate(32, 40, 51));
    palette.setColor(QPalette::ToolTipText, slate(236, 241, 247));
    palette.setColor(QPalette::Text, slate(236, 241, 247));
    palette.setColor(QPalette::Button, slate(30, 39, 51));
    palette.setColor(QPalette::ButtonText, slate(236, 241, 247));
    palette.setColor(QPalette::BrightText, slate(255, 103, 103));
    palette.setColor(QPalette::Link, slate(83, 184, 199));
    palette.setColor(QPalette::Highlight, slate(57, 134, 153));
    palette.setColor(QPalette::HighlightedText, QColor(Qt::white));

    setDisabled(palette, QPalette::WindowText, slate(107, 118, 132));
    setDisabled(palette, QPalette::Text, slate(107, 118, 132));
    setDisabled(palette, QPalette::ButtonText, slate(107, 118, 132));
    setDisabled(palette, QPalette::Highlight, slate(42, 52, 64));
    setDisabled(palette, QPalette::HighlightedText, slate(145, 154, 166));
    app.setPalette(palette);

    QFile styleFile(":/ui/style.qss");
    if (styleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QString::fromUtf8(styleFile.readAll()));
    }
}

}
