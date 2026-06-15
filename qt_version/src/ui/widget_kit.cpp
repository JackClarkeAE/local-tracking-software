#include "widget_kit.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace {

const char* roleName(WidgetKit::ButtonRole role) {
    switch (role) {
    case WidgetKit::ButtonRole::Primary: return "primary";
    case WidgetKit::ButtonRole::Destructive: return "destructive";
    case WidgetKit::ButtonRole::Recording: return "recording";
    case WidgetKit::ButtonRole::Ghost: return "ghost";
    case WidgetKit::ButtonRole::Default:
    default: return "default";
    }
}

}

namespace WidgetKit {

void repolish(QWidget* widget) {
    if (!widget || !widget->style()) return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

QFrame* separator(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setProperty("dsSeparator", true);
    return line;
}

QWidget* sectionHeader(const QString& title, QWidget* parent) {
    auto* row = new QWidget(parent);
    row->setProperty("dsSectionHeader", true);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 6, 0, 2);
    layout->setSpacing(8);

    auto* label = new QLabel(title, row);
    label->setProperty("dsSectionTitle", true);
    layout->addWidget(label);
    layout->addWidget(separator(row), 1);
    return row;
}

void addSectionHeader(QVBoxLayout* layout, const QString& title) {
    if (!layout) return;
    layout->addWidget(sectionHeader(title));
}

QFrame* panel(const QString& title, QWidget* parent) {
    auto* frame = new QFrame(parent);
    frame->setFrameShape(QFrame::NoFrame);
    frame->setProperty("dsPanel", true);

    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(10, 8, 10, 10);
    layout->setSpacing(6);

    if (!title.isEmpty()) {
        auto* label = new QLabel(title, frame);
        label->setProperty("dsPanelTitle", true);
        layout->addWidget(label);
        layout->addWidget(separator(frame));
    }
    return frame;
}

QPushButton* button(const QString& text, ButtonRole role, QWidget* parent) {
    auto* pushButton = new QPushButton(text, parent);
    pushButton->setMinimumHeight(24);
    setButtonRole(pushButton, role);
    return pushButton;
}

QLabel* statusPill(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("dsStatusPill", "neutral");
    return label;
}

void setButtonRole(QPushButton* button, ButtonRole role) {
    if (!button) return;
    button->setProperty("role", roleName(role));
    repolish(button);
}

}
