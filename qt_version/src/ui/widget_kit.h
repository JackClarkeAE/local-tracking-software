#pragma once

#include <QString>

class QLabel;
class QPushButton;
class QFrame;
class QVBoxLayout;
class QWidget;

namespace WidgetKit {

enum class ButtonRole {
    Default,
    Primary,
    Destructive,
    Recording,
    Ghost
};

QFrame* separator(QWidget* parent = nullptr);
QWidget* sectionHeader(const QString& title, QWidget* parent = nullptr);
void addSectionHeader(QVBoxLayout* layout, const QString& title);

QFrame* panel(const QString& title = QString(), QWidget* parent = nullptr);
QPushButton* button(const QString& text, ButtonRole role = ButtonRole::Default,
                    QWidget* parent = nullptr);
QLabel* statusPill(const QString& text = QString(), QWidget* parent = nullptr);

void setButtonRole(QPushButton* button, ButtonRole role);
void repolish(QWidget* widget);

}
