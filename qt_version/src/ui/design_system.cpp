#include "design_system.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QIODevice>
#include <QPalette>
#include <QStyleFactory>

namespace {

QColor rgb(int r, int g, int b) {
    return QColor(r, g, b);
}

void setDisabled(QPalette& palette, QPalette::ColorRole role, const QColor& color) {
    palette.setColor(QPalette::Disabled, role, color);
}

// Upstream dark palette ("Clinical Slate").
QPalette slatePalette() {
    QPalette palette;
    palette.setColor(QPalette::Window, rgb(16, 20, 26));
    palette.setColor(QPalette::WindowText, rgb(236, 241, 247));
    palette.setColor(QPalette::Base, rgb(18, 24, 32));
    palette.setColor(QPalette::AlternateBase, rgb(24, 31, 41));
    palette.setColor(QPalette::ToolTipBase, rgb(32, 40, 51));
    palette.setColor(QPalette::ToolTipText, rgb(236, 241, 247));
    palette.setColor(QPalette::Text, rgb(236, 241, 247));
    palette.setColor(QPalette::Button, rgb(30, 39, 51));
    palette.setColor(QPalette::ButtonText, rgb(236, 241, 247));
    palette.setColor(QPalette::BrightText, rgb(255, 103, 103));
    palette.setColor(QPalette::Link, rgb(83, 184, 199));
    palette.setColor(QPalette::Highlight, rgb(57, 134, 153));
    palette.setColor(QPalette::HighlightedText, QColor(Qt::white));

    setDisabled(palette, QPalette::WindowText, rgb(107, 118, 132));
    setDisabled(palette, QPalette::Text, rgb(107, 118, 132));
    setDisabled(palette, QPalette::ButtonText, rgb(107, 118, 132));
    setDisabled(palette, QPalette::Highlight, rgb(42, 52, 64));
    setDisabled(palette, QPalette::HighlightedText, rgb(145, 154, 166));
    return palette;
}

// YCTS light clinical palette: soft neutral surfaces, deep teal accent,
// high-contrast ink. Mirrors the tokens in style_clinical.qss.
QPalette clinicalPalette() {
    QPalette palette;
    palette.setColor(QPalette::Window, rgb(243, 246, 249));
    palette.setColor(QPalette::WindowText, rgb(22, 33, 46));
    palette.setColor(QPalette::Base, rgb(255, 255, 255));
    palette.setColor(QPalette::AlternateBase, rgb(246, 249, 251));
    palette.setColor(QPalette::ToolTipBase, rgb(22, 33, 46));
    palette.setColor(QPalette::ToolTipText, rgb(255, 255, 255));
    palette.setColor(QPalette::Text, rgb(22, 33, 46));
    palette.setColor(QPalette::Button, rgb(255, 255, 255));
    palette.setColor(QPalette::ButtonText, rgb(22, 33, 46));
    palette.setColor(QPalette::BrightText, rgb(200, 16, 46));
    palette.setColor(QPalette::Link, rgb(0, 110, 130));
    palette.setColor(QPalette::Highlight, rgb(0, 110, 130));
    palette.setColor(QPalette::HighlightedText, QColor(Qt::white));

    setDisabled(palette, QPalette::WindowText, rgb(150, 161, 172));
    setDisabled(palette, QPalette::Text, rgb(150, 161, 172));
    setDisabled(palette, QPalette::ButtonText, rgb(150, 161, 172));
    setDisabled(palette, QPalette::Highlight, rgb(205, 214, 222));
    setDisabled(palette, QPalette::HighlightedText, rgb(120, 130, 140));
    return palette;
}

}

namespace DesignSystem {

void apply(QApplication& app, Theme theme) {
    app.setStyle(QStyleFactory::create("Fusion"));

    QFont font("Segoe UI", theme == Theme::Clinical ? 10 : 9);
    font.setStyleHint(QFont::SansSerif);
    app.setFont(font);

    app.setPalette(theme == Theme::Clinical ? clinicalPalette() : slatePalette());

    const QString sheet = theme == Theme::Clinical ? ":/ui/style_clinical.qss"
                                                   : ":/ui/style.qss";
    QFile styleFile(sheet);
    if (styleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QString::fromUtf8(styleFile.readAll()));
    }
}

}
